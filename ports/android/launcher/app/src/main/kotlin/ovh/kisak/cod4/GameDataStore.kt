package ovh.kisak.cod4

import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Finds, validates and imports the player's own Call of Duty 4 data.
 *
 * No game content is shipped with this app and none is downloaded. The player
 * points the launcher at a copy of the PC installation they already own and
 * the files are copied into app-private storage, which is the only place the
 * engine can open them with plain POSIX calls - scoped storage means a
 * content:// URI is not a path, and the engine's file layer wants paths.
 *
 * What the engine actually needs:
 *   main/*.iwd        the base game archives
 *   zone/<lang>/*.ff  the fastfiles for one language
 *   localization.txt  names that language
 * Everything else in a retail install (binaries, Bink movies, the launcher)
 * is ignored.
 */
object GameDataStore {

    private const val TAG = "KisakCOD-data"

    /** Minimum plausible size of a complete install, as a sanity check. */
    private const val MINIMUM_TOTAL_BYTES = 3L * 1024 * 1024 * 1024

    data class Status(
        val ready: Boolean,
        val missing: List<String>,
        val language: String?,
        val totalBytes: Long,
    )

    private val importing = AtomicBoolean(false)

    fun root(context: Context): File = File(context.filesDir, "cod4")

    fun mainDir(context: Context): File = File(root(context), "main")

    fun zoneDir(context: Context): File = File(root(context), "zone")

    /**
     * Checks what is present. Deliberately cheap - it runs on every launch, so
     * it counts files and sizes rather than opening archives.
     */
    fun status(context: Context): Status {
        val root = root(context)
        val missing = mutableListOf<String>()
        var total = 0L

        val main = mainDir(context)
        val iwds = main.listFiles { file -> file.extension.equals("iwd", ignoreCase = true) }.orEmpty()
        if (iwds.isEmpty()) {
            missing += "main/*.iwd"
        }
        iwds.forEach { total += it.length() }

        val localization = File(root, "localization.txt")
        val language = if (localization.isFile) {
            // The file is a single token, e.g. "english". Anything longer is
            // a stray file rather than the real one.
            localization.readText().trim().lowercase().takeIf { it.isNotEmpty() && it.length < 32 }
        } else {
            null
        }
        if (language == null) {
            missing += "localization.txt"
        }

        val zone = zoneDir(context)
        val zoneLanguageDir = language?.let { File(zone, it) }
        val fastfiles = zoneLanguageDir?.listFiles { file ->
            file.extension.equals("ff", ignoreCase = true)
        }.orEmpty()
        if (fastfiles.isEmpty()) {
            missing += if (language != null) "zone/$language/*.ff" else "zone/<language>/*.ff"
        }
        fastfiles.forEach { total += it.length() }

        // Common fastfiles live alongside the language directory.
        File(zone, "common").listFiles { file -> file.extension.equals("ff", ignoreCase = true) }
            ?.forEach { total += it.length() }

        return Status(
            ready = missing.isEmpty() && total > 0,
            missing = missing,
            language = language,
            totalBytes = total,
        )
    }

    /**
     * Copies a selected folder tree into app storage.
     *
     * Reports progress as bytes copied out of bytes expected. The caller runs
     * this off the main thread; the import is tens of gigabytes of IO on a
     * full install and takes minutes even on fast storage.
     */
    fun import(
        context: Context,
        treeUri: Uri,
        onProgress: (copied: Long, total: Long, currentFile: String) -> Unit,
        shouldCancel: () -> Boolean,
    ): Result<Status> {
        if (!importing.compareAndSet(false, true)) {
            return Result.failure(IllegalStateException("An import is already running"))
        }
        try {
            val source = DocumentFile.fromTreeUri(context, treeUri)
                ?: return Result.failure(IllegalArgumentException("That folder could not be opened"))

            // The player may have selected the install folder itself or its
            // parent. Looking one level down for "main" covers both without
            // making them pick again.
            val installRoot = locateInstallRoot(source)
                ?: return Result.failure(
                    IllegalArgumentException(
                        "No main folder in there. Choose the folder that contains main and zone."
                    )
                )

            val wanted = collectWantedFiles(installRoot)
            if (wanted.isEmpty()) {
                return Result.failure(IllegalArgumentException("That folder has no .iwd or .ff files in it"))
            }

            val total = wanted.sumOf { it.file.length() }
            if (total < MINIMUM_TOTAL_BYTES) {
                Log.w(TAG, "only $total bytes selected; a full install is larger")
            }

            val destinationRoot = root(context)
            destinationRoot.mkdirs()

            var copied = 0L
            for (entry in wanted) {
                if (shouldCancel()) {
                    return Result.failure(InterruptedException("Import cancelled"))
                }
                val destination = File(destinationRoot, entry.relativePath)
                destination.parentFile?.mkdirs()

                // Skip files already present at the same size. An import
                // interrupted by the system killing the app can then be
                // resumed instead of restarted.
                if (destination.isFile && destination.length() == entry.file.length()) {
                    copied += entry.file.length()
                    onProgress(copied, total, entry.relativePath)
                    continue
                }

                val partial = File(destination.parentFile, destination.name + ".part")
                context.contentResolver.openInputStream(entry.file.uri).use { input ->
                    if (input == null) {
                        return Result.failure(java.io.IOException("Could not read ${entry.relativePath}"))
                    }
                    FileOutputStream(partial).use { output ->
                        val buffer = ByteArray(1 shl 20)
                        while (true) {
                            if (shouldCancel()) {
                                partial.delete()
                                return Result.failure(InterruptedException("Import cancelled"))
                            }
                            val read = input.read(buffer)
                            if (read <= 0) break
                            output.write(buffer, 0, read)
                            copied += read
                            onProgress(copied, total, entry.relativePath)
                        }
                        output.fd.sync()
                    }
                }
                // Rename only once the whole file is on disk, so an interrupted
                // copy never looks like a complete one.
                if (!partial.renameTo(destination)) {
                    partial.delete()
                    return Result.failure(java.io.IOException("Could not finish ${entry.relativePath}"))
                }
            }

            return Result.success(status(context))
        } catch (error: Exception) {
            Log.e(TAG, "import failed", error)
            return Result.failure(error)
        } finally {
            importing.set(false)
        }
    }

    private data class Entry(val file: DocumentFile, val relativePath: String)

    private fun locateInstallRoot(source: DocumentFile): DocumentFile? {
        if (source.findFile("main")?.isDirectory == true) return source
        return source.listFiles()
            .firstOrNull { it.isDirectory && it.findFile("main")?.isDirectory == true }
    }

    /**
     * Walks the install and picks out only what the engine reads. A retail
     * folder holds several gigabytes of Windows binaries and Bink movies that
     * would otherwise be copied for nothing.
     */
    private fun collectWantedFiles(root: DocumentFile): List<Entry> {
        val result = mutableListOf<Entry>()

        root.findFile("localization.txt")?.takeIf { it.isFile }?.let {
            result += Entry(it, "localization.txt")
        }

        root.findFile("main")?.takeIf { it.isDirectory }?.let { main ->
            main.listFiles().forEach { file ->
                val name = file.name ?: return@forEach
                if (file.isFile && (name.endsWith(".iwd", true) || name.endsWith(".cfg", true))) {
                    result += Entry(file, "main/$name")
                }
            }
            // Mods and custom maps live one level deeper.
            main.listFiles().filter { it.isDirectory }.forEach { modDir ->
                val modName = modDir.name ?: return@forEach
                modDir.listFiles().forEach { file ->
                    val name = file.name ?: return@forEach
                    if (file.isFile && (name.endsWith(".iwd", true) || name.endsWith(".ff", true))) {
                        result += Entry(file, "main/$modName/$name")
                    }
                }
            }
        }

        root.findFile("zone")?.takeIf { it.isDirectory }?.let { zone ->
            zone.listFiles().filter { it.isDirectory }.forEach { languageDir ->
                val language = languageDir.name ?: return@forEach
                languageDir.listFiles().forEach { file ->
                    val name = file.name ?: return@forEach
                    if (file.isFile && name.endsWith(".ff", true)) {
                        result += Entry(file, "zone/$language/$name")
                    }
                }
            }
        }

        // Converted cutscenes, if the player ran the conversion script.
        root.findFile("video")?.takeIf { it.isDirectory }?.let { video ->
            video.listFiles().forEach { file ->
                val name = file.name ?: return@forEach
                if (file.isFile && (name.endsWith(".mp4", true) || name.endsWith(".mp3", true))) {
                    result += Entry(file, "video/$name")
                }
            }
        }

        return result
    }

    /** Frees the imported data. Offered in settings, because it is tens of gigabytes. */
    fun clear(context: Context): Boolean = root(context).deleteRecursively()
}
