package ovh.kisak.cod4

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.os.Build
import android.provider.MediaStore
import androidx.core.content.FileProvider
import java.io.File
import java.io.RandomAccessFile
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The engine's log file, from the Java side.
 *
 * The launcher owns the location rather than asking native code for it. A
 * crash takes the whole process down, so by the time the player is looking at
 * the launcher again this is a fresh process in which no engine library has
 * necessarily been loaded and no storage root has been reported - but the
 * file from the run that died is still on disk, and reading it must not
 * depend on any of that.
 *
 * Internal storage, not the external one: `getExternalFilesDir` is on a
 * FUSE-emulated volume where a write from a signal handler can block, and the
 * handler runs on a process that is already dying.
 */
object GameLog {

    private const val DIRECTORY = "logs"
    private const val CURRENT = "kisakcod-log.txt"
    private const val PREVIOUS = "kisakcod-log-previous.txt"

    /** Marker written by the native crash handler. */
    private const val CRASH_MARKER = "==== CRASH ===="

    /**
     * How much of the file the viewer and the clipboard take. A long play
     * session writes a few megabytes, almost all of it asset loading, and the
     * interesting part is always at the end. Putting several megabytes of
     * text into a TextView or onto the clipboard would hang the UI thread or
     * be silently dropped by the clipboard service.
     */
    private const val TAIL_LIMIT = 256 * 1024

    fun directory(context: Context): File =
        File(context.filesDir, DIRECTORY).apply { mkdirs() }

    fun current(context: Context): File = File(directory(context), CURRENT)

    fun previous(context: Context): File = File(directory(context), PREVIOUS)

    /**
     * Moves the last run's log aside. Called just before the engine starts,
     * never when the launcher opens: the file from a run that crashed has to
     * survive the player going back to the launcher to look at it.
     */
    fun rotate(context: Context) {
        val current = current(context)
        if (!current.exists() || current.length() == 0L) return
        val previous = previous(context)
        previous.delete()
        if (!current.renameTo(previous)) {
            // Same directory, so a rename only fails in odd circumstances.
            // Losing the older log is better than appending this run to it.
            current.delete()
        }
    }

    fun exists(context: Context): Boolean = current(context).length() > 0

    /** True when the most recent run ended in a native crash. */
    fun lastRunCrashed(context: Context): Boolean =
        read(context, TAIL_LIMIT).contains(CRASH_MARKER)

    /**
     * The tail of the log, as text. Reads from the end rather than loading
     * the file and dropping most of it, so a large log costs nothing extra.
     */
    fun read(context: Context, limit: Int = TAIL_LIMIT): String = read(current(context), limit)

    fun read(file: File, limit: Int = TAIL_LIMIT): String {
        if (!file.exists()) return ""
        return try {
            RandomAccessFile(file, "r").use { handle ->
                val length = handle.length()
                val from = maxOf(0L, length - limit)
                handle.seek(from)
                val bytes = ByteArray((length - from).toInt())
                handle.readFully(bytes)
                val text = String(bytes, Charsets.UTF_8)
                if (from > 0L) {
                    "[the first ${(from / 1024)} KB are not shown]\n\n" + text.substringAfter('\n')
                } else {
                    text
                }
            }
        } catch (error: Exception) {
            "Could not read ${file.absolutePath}: ${error.message}"
        }
    }

    fun copyToClipboard(context: Context, text: String) {
        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("KisakCOD log", text))
    }

    /**
     * A share sheet with the whole file attached, which is how a log of any
     * size actually reaches a bug report. Needs the FileProvider declared in
     * the manifest: internal storage is not readable by another app, so the
     * file is handed over as a content:// URI with a temporary grant.
     */
    fun shareIntent(context: Context, file: File = current(context)): Intent? {
        if (!file.exists() || file.length() == 0L) return null
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.logs", file)
        val share = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, exportName())
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        return Intent.createChooser(share, null)
    }

    /**
     * Writes a copy into the device's Downloads folder, where a file manager
     * or a desktop over USB can pick it up without the app being involved.
     * Returns what to tell the player, or null if it did not work.
     */
    fun saveToDownloads(context: Context, file: File = current(context)): String? {
        if (!file.exists() || file.length() == 0L) return null
        val name = exportName()
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, name)
                    put(MediaStore.Downloads.MIME_TYPE, "text/plain")
                    put(MediaStore.Downloads.IS_PENDING, 1)
                }
                val resolver = context.contentResolver
                val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: return null
                resolver.openOutputStream(uri)?.use { output -> file.inputStream().use { it.copyTo(output) } }
                values.clear()
                values.put(MediaStore.Downloads.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
                "Downloads/$name"
            } else {
                // Before Android 10 there is no Downloads collection to insert
                // into, and writing anywhere else on the shared volume needs a
                // runtime permission. The app-specific external directory needs
                // none and is still reachable over USB.
                val directory = context.getExternalFilesDir(null) ?: return null
                val copy = File(directory, name)
                file.copyTo(copy, overwrite = true)
                copy.absolutePath
            }
        } catch (error: Exception) {
            null
        }
    }

    private fun exportName(): String {
        val stamp = SimpleDateFormat("yyyy-MM-dd-HHmm", Locale.US).format(Date())
        return "kisakcod-log-$stamp.txt"
    }
}
