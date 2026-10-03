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
 *   main/             the base game archives, .iwd
 *   zone/<lang>/      the fastfiles for one language, .ff
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
        /** What was actually found, so an incomplete import can be diagnosed. */
        val detail: String,
    )

    private val importing = AtomicBoolean(false)

    fun root(context: Context): File = File(context.filesDir, "cod4")

    fun mainDir(context: Context): File = File(root(context), "main")

    fun zoneDir(context: Context): File = File(root(context), "zone")

    /**
     * Checks what is present, and repairs localization.txt if it has to.
     *
     * Deliberately cheap - it runs on every launch, so it counts files and
     * sizes rather than opening archives. The one write it can make happens
     * at most once, when the declared language and the zone directory do not
     * already agree.
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

        // Every zone subdirectory that holds fastfiles. DB_BuildOSPath builds
        // "zone/<language>/<name>.ff", so a language is only usable if its
        // directory is the one with the files in it.
        val byLanguage = fastfilesByLanguage(zoneDir(context))
        byLanguage.values.forEach { files -> files.forEach { total += it.length() } }

        // Matched case-insensitively but carried through exactly as spelled on
        // disk: internal storage is case-sensitive, so writing "english" when
        // the directory is "English" would send DB_BuildOSPath somewhere that
        // does not exist.
        val declared = readDeclaredLanguage(root)
        val language = byLanguage.keys.firstOrNull { it.equals(declared, ignoreCase = true) }
            ?: byLanguage.entries.maxByOrNull { entry -> entry.value.size }?.key

        if (byLanguage.isEmpty()) {
            missing += if (declared != null) "zone/$declared/*.ff" else "zone/<language>/*.ff"
        }

        // Win_InitLocalization opens localization.txt directly and asserts when
        // it is absent, and DB_BuildOSPath takes the language from it, so it has
        // to agree with the directory the fastfiles are in. Not every copy of
        // the game carries the file, and some carry one naming a language that
        // was never installed; in both cases the zone directory already says
        // what the language is, so write it rather than refusing to start.
        if (language != null && declared != language) {
            if (writeDeclaredLanguage(root, language)) {
                Log.i(TAG, "wrote localization.txt for '$language' (was ${declared ?: "absent"})")
            } else {
                missing += "localization.txt"
            }
        } else if (language == null && declared == null) {
            missing += "localization.txt"
        }

        return Status(
            ready = missing.isEmpty() && total > 0,
            missing = missing,
            language = language,
            totalBytes = total,
            detail = describe(iwds.size, byLanguage, declared),
        )
    }

    private fun describe(
        iwdCount: Int,
        byLanguage: Map<String, List<File>>,
        declared: String?,
    ): String = buildString {
        append("main: ").append(iwdCount).append(" .iwd")
        append(" · zone: ")
        if (byLanguage.isEmpty()) {
            append("no .ff found")
        } else {
            append(byLanguage.entries.joinToString(", ") { "${it.key} ${it.value.size} .ff" })
        }
        append(" · localization.txt: ").append(declared ?: "absent")
    }

    /**
     * Fastfiles per zone subdirectory, skipping directories that have none.
     * Keyed by the directory's real name, case and all.
     */
    private fun fastfilesByLanguage(zone: File): Map<String, List<File>> {
        val result = linkedMapOf<String, List<File>>()
        zone.listFiles()?.sortedBy { it.name }?.forEach { child ->
            if (!child.isDirectory) return@forEach
            val fastfiles = child.listFiles { file ->
                file.isFile && file.extension.equals("ff", ignoreCase = true)
            }.orEmpty().toList()
            if (fastfiles.isNotEmpty()) {
                result[child.name] = fastfiles
            }
        }
        return result
    }

    /** The language token on the first line, as written, or null if absent. */
    private fun readDeclaredLanguage(root: File): String? {
        val file = File(root, "localization.txt")
        if (!file.isFile) return null
        return runCatching {
            file.useLines { lines -> lines.firstOrNull() }
                ?.trim()
                ?.takeIf { it.isNotEmpty() && it.length < 32 && it.all { c -> c.isLetter() } }
        }.getOrNull()
    }

    /**
     * Writes a minimal localization.txt.
     *
     * Win_InitLocalization reads the language up to the first newline and
     * points its string table at whatever follows, so the trailing newline is
     * not optional: without it the table pointer is left null.
     */
    private fun writeDeclaredLanguage(root: File, language: String): Boolean = runCatching {
        root.mkdirs()
        File(root, "localization.txt").writeText("$language\n")
        true
    }.getOrElse {
        Log.e(TAG, "could not write localization.txt", it)
        false
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

    /**
     * Case-insensitive child lookup.
     *
     * DocumentFile.findFile compares names exactly. A copy made on Windows and
     * moved to a phone can easily arrive as "Main" or "Localization.txt", and
     * on an sdcard formatted FAT the case is whatever the first writer chose.
     * The engine's own file layer folds case, so the importer has to as well.
     */
    private fun DocumentFile.child(name: String): DocumentFile? =
        listFiles().firstOrNull { it.name.equals(name, ignoreCase = true) }

    private fun locateInstallRoot(source: DocumentFile): DocumentFile? {
        if (source.child("main")?.isDirectory == true) return source
        return source.listFiles()
            .firstOrNull { it.isDirectory && it.child("main")?.isDirectory == true }
    }

    /**
     * Walks the install and picks out only what the engine reads. A retail
     * folder holds several gigabytes of Windows binaries and Bink movies that
     * would otherwise be copied for nothing.
     */
    private fun collectWantedFiles(root: DocumentFile): List<Entry> {
        val result = mutableListOf<Entry>()

        root.child("localization.txt")?.takeIf { it.isFile }?.let {
            result += Entry(it, "localization.txt")
        }

        root.child("main")?.takeIf { it.isDirectory }?.let { main ->
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

        root.child("zone")?.takeIf { it.isDirectory }?.let { zone ->
            // Fastfiles sitting straight in zone/ rather than in a language
            // directory. DB_BuildOSPath only ever looks in zone/<language>/,
            // so they have to be filed under one. English is the assumption;
            // status() then rewrites localization.txt to whichever directory
            // actually ended up with the files, so a wrong guess corrects
            // itself rather than leaving the two disagreeing.
            zone.listFiles()
                .filter { it.isFile && it.name?.endsWith(".ff", true) == true }
                .forEach { file -> result += Entry(file, "zone/english/${file.name}") }
            // Language directories, and anything nested inside them: some
            // repacks keep the map fastfiles one level further down.
            zone.listFiles().filter { it.isDirectory }.forEach { languageDir ->
                val language = languageDir.name ?: return@forEach
                collectFastfiles(languageDir, "zone/$language", result)
            }
        }

        // Converted cutscenes, if the player ran the conversion script.
        root.child("video")?.takeIf { it.isDirectory }?.let { video ->
            video.listFiles().forEach { file ->
                val name = file.name ?: return@forEach
                if (file.isFile && (name.endsWith(".mp4", true) || name.endsWith(".mp3", true))) {
                    result += Entry(file, "video/$name")
                }
            }
        }

        return result
    }

    /** Collects .ff files at any depth, flattened into the language directory. */
    private fun collectFastfiles(directory: DocumentFile, prefix: String, into: MutableList<Entry>) {
        directory.listFiles().forEach { file ->
            val name = file.name ?: return@forEach
            when {
                file.isFile && name.endsWith(".ff", true) -> into += Entry(file, "$prefix/$name")
                file.isDirectory -> collectFastfiles(file, prefix, into)
            }
        }
    }

    /** Frees the imported data. Offered in settings, because it is tens of gigabytes. */
    fun clear(context: Context): Boolean = root(context).deleteRecursively()
}
