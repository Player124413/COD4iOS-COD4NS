package ovh.kisak.cod4

import android.graphics.Typeface
import android.os.Bundle
import android.text.format.Formatter
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.io.File
import java.text.DateFormat
import java.util.Date
import kotlin.concurrent.thread

/**
 * Shows the engine log and gets it off the device.
 *
 * A crash report is only useful if the player can actually send it, and on a
 * phone that means one of three things: the clipboard, a share sheet, or a
 * file somewhere a desktop can see over USB. All three are here, because
 * which one works depends on what the player has to hand.
 */
class LogActivity : AppCompatActivity() {

    private lateinit var headerText: TextView
    private lateinit var bodyText: TextView
    private lateinit var switchButton: Button

    /** Which file is on screen: the last run, or the one before it. */
    private var showingPrevious = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildLayout())
        load()
    }

    private fun file(): File =
        if (showingPrevious) GameLog.previous(this) else GameLog.current(this)

    private fun buildLayout(): View {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(40, 48, 40, 32)
        }

        content.addView(TextView(this).apply {
            setText(R.string.log_title)
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })

        headerText = TextView(this).apply {
            textSize = 12f
            setPadding(0, 12, 0, 16)
        }
        content.addView(headerText)

        // Two rows rather than one: three buttons plus the file switch do not
        // fit across a phone in portrait without the labels being clipped.
        val firstRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        firstRow.addView(button(R.string.log_copy) { copy() }, rowParams())
        firstRow.addView(button(R.string.log_share) { share() }, rowParams())
        content.addView(firstRow)

        val secondRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        secondRow.addView(button(R.string.log_save) { save() }, rowParams())
        switchButton = button(R.string.log_show_previous) { togglePrevious() }
        secondRow.addView(switchButton, rowParams())
        content.addView(secondRow)

        bodyText = TextView(this).apply {
            textSize = 11f
            typeface = Typeface.MONOSPACE
            // Selectable so a player can also pull out just the few lines that
            // matter, instead of pasting a quarter of a megabyte into a chat.
            setTextIsSelectable(true)
            setPadding(0, 16, 0, 0)
        }

        // The log has long lines - paths, and backtrace frames with a library
        // name and an offset - and wrapping them makes the stack hard to read.
        val sideways = HorizontalScrollView(this).apply {
            isHorizontalScrollBarEnabled = true
            addView(bodyText)
        }

        val vertical = ScrollView(this).apply {
            isFillViewport = true
            addView(sideways)
        }
        content.addView(vertical, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        // targetSdk 35 draws edge to edge, so the content would otherwise sit
        // under the status bar and behind the gesture handle.
        ViewCompat.setOnApplyWindowInsetsListener(content) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.setPadding(40 + bars.left, 48 + bars.top, 40 + bars.right, 32 + bars.bottom)
            insets
        }
        return content
    }

    private fun button(labelId: Int, onClick: () -> Unit) = Button(this).apply {
        setText(labelId)
        textSize = 13f
        setOnClickListener { onClick() }
    }

    private fun rowParams() = LinearLayout.LayoutParams(
        0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f
    ).apply { gravity = Gravity.CENTER_VERTICAL }

    // --- contents ------------------------------------------------------------

    private fun load() {
        val file = file()
        switchButton.setText(
            if (showingPrevious) R.string.log_show_latest else R.string.log_show_previous
        )
        switchButton.isEnabled = showingPrevious || GameLog.previous(this).length() > 0

        if (!file.exists() || file.length() == 0L) {
            headerText.text = getString(R.string.log_empty_header)
            bodyText.setText(R.string.log_empty)
            return
        }

        headerText.text = getString(
            R.string.log_header,
            Formatter.formatShortFileSize(this, file.length()),
            DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                .format(Date(file.lastModified())),
            file.absolutePath,
        )

        // Reading a few hundred kilobytes off internal storage is quick, but
        // not quick enough to do on the thread that is drawing the screen.
        bodyText.setText(R.string.log_loading)
        thread {
            val text = GameLog.read(file)
            runOnUiThread { bodyText.text = text }
        }
    }

    private fun togglePrevious() {
        showingPrevious = !showingPrevious
        load()
    }

    private fun copy() {
        val text = bodyText.text?.toString().orEmpty()
        if (text.isEmpty()) return
        GameLog.copyToClipboard(this, text)
        // Android 13 and later show their own copy confirmation, and a toast
        // on top of it is just noise.
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(this, R.string.log_copied, Toast.LENGTH_SHORT).show()
        }
    }

    private fun share() {
        val intent = GameLog.shareIntent(this, file())
        if (intent == null) {
            Toast.makeText(this, R.string.log_empty, Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(intent)
    }

    private fun save() {
        thread {
            val where = GameLog.saveToDownloads(this, file())
            runOnUiThread {
                if (where != null) {
                    Toast.makeText(this, getString(R.string.log_saved, where), Toast.LENGTH_LONG).show()
                } else {
                    Toast.makeText(this, R.string.log_save_failed, Toast.LENGTH_LONG).show()
                }
            }
        }
    }
}
