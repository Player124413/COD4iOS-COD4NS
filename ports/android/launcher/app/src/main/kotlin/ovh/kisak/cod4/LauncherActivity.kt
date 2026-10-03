package ovh.kisak.cod4

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.format.Formatter
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import kotlin.concurrent.thread

/**
 * The launcher.
 *
 * Its job is everything that has to happen before the engine can start, and
 * nothing that happens afterwards:
 *
 *  - explain that the player supplies their own game data, and import it
 *  - say plainly what is missing when the import is incomplete
 *  - choose single player or multiplayer, which decides which engine library
 *    the next process loads
 *  - expose the handful of performance settings that override what the device
 *    profile picked
 *
 * Written in views rather than Compose on purpose: this screen is shown for a
 * few seconds before a game that wants every megabyte of memory and every
 * millisecond of startup, and Compose's runtime would be loaded into the same
 * process that then has to hold a level in RAM.
 */
class LauncherActivity : AppCompatActivity() {

    private lateinit var settings: Settings
    private lateinit var statusText: TextView
    private lateinit var playButton: Button
    private lateinit var importButton: Button
    private lateinit var progressBar: ProgressBar
    private lateinit var progressText: TextView

    @Volatile
    private var cancelImport = false

    private val pickFolder = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        if (uri == null) return@registerForActivityResult
        // Persist the permission so a retry after a restart does not need the
        // player to pick the folder again.
        runCatching {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        }
        startImport(uri)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        setContentView(buildLayout())
        refreshStatus()
    }

    override fun onResume() {
        super.onResume()
        refreshStatus()
    }

    // --- layout --------------------------------------------------------------

    private fun buildLayout(): View {
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 64, 48, 64)
        }

        content.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 26f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        })

        content.addView(TextView(this).apply {
            setText(R.string.data_notice)
            textSize = 13f
            setPadding(0, 24, 0, 24)
        })

        statusText = TextView(this).apply {
            textSize = 14f
            setPadding(0, 0, 0, 24)
        }
        content.addView(statusText)

        progressBar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            visibility = View.GONE
        }
        content.addView(progressBar)

        progressText = TextView(this).apply {
            textSize = 12f
            visibility = View.GONE
            setPadding(0, 8, 0, 16)
        }
        content.addView(progressText)

        importButton = Button(this).apply {
            setText(R.string.choose_folder)
            setOnClickListener { pickFolder.launch(null) }
        }
        content.addView(importButton)

        playButton = Button(this).apply {
            setText(R.string.play)
            isEnabled = false
            setOnClickListener { launchGame() }
        }
        content.addView(playButton)

        content.addView(divider())

        // --- mode ---
        content.addView(sectionLabel(R.string.mode))
        val modeGroup = RadioGroup(this).apply {
            orientation = LinearLayout.HORIZONTAL
            val sp = RadioButton(this@LauncherActivity).apply {
                id = View.generateViewId()
                setText(R.string.single_player)
            }
            val mp = RadioButton(this@LauncherActivity).apply {
                id = View.generateViewId()
                setText(R.string.multiplayer)
            }
            addView(sp)
            addView(mp)
            check(if (settings.engineMode == EngineBridge.Mode.SinglePlayer) sp.id else mp.id)
            setOnCheckedChangeListener { _, checkedId ->
                settings.engineMode = if (checkedId == sp.id) {
                    EngineBridge.Mode.SinglePlayer
                } else {
                    EngineBridge.Mode.Multiplayer
                }
            }
        }
        content.addView(modeGroup)
        content.addView(hint(R.string.mode_hint))

        content.addView(divider())

        // --- performance ---
        content.addView(sectionLabel(R.string.performance))

        val scaleLabel = TextView(this).apply { textSize = 13f }
        val scaleBar = SeekBar(this).apply {
            max = 8 // 0.60 to 1.00 in steps of 0.05
            progress = ((settings.renderScale - 0.6f) / 0.05f).toInt().coerceIn(0, 8)
            isEnabled = !settings.dynamicResolution
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar?, value: Int, fromUser: Boolean) {
                    val scale = 0.6f + value * 0.05f
                    settings.renderScale = scale
                    scaleLabel.text = getString(R.string.render_scale, (scale * 100).toInt())
                }

                override fun onStartTrackingTouch(bar: SeekBar?) = Unit
                override fun onStopTrackingTouch(bar: SeekBar?) = Unit
            })
        }
        scaleLabel.text = getString(R.string.render_scale, (settings.renderScale * 100).toInt())

        content.addView(CheckBox(this).apply {
            setText(R.string.dynamic_resolution)
            isChecked = settings.dynamicResolution
            setOnCheckedChangeListener { _, checked ->
                settings.dynamicResolution = checked
                scaleBar.isEnabled = !checked
            }
        })
        content.addView(hint(R.string.dynamic_resolution_hint))
        content.addView(scaleLabel)
        content.addView(scaleBar)

        content.addView(CheckBox(this).apply {
            setText(R.string.touch_controls)
            isChecked = settings.touchControls
            setOnCheckedChangeListener { _, checked -> settings.touchControls = checked }
        })
        content.addView(hint(R.string.touch_controls_hint))

        content.addView(CheckBox(this).apply {
            setText(R.string.perf_overlay)
            isChecked = settings.showPerfOverlay
            setOnCheckedChangeListener { _, checked -> settings.showPerfOverlay = checked }
        })

        content.addView(divider())

        content.addView(Button(this).apply {
            setText(R.string.delete_data)
            setOnClickListener {
                androidx.appcompat.app.AlertDialog.Builder(this@LauncherActivity)
                    .setTitle(R.string.delete_data)
                    .setMessage(R.string.delete_data_confirm)
                    .setPositiveButton(R.string.delete) { _, _ ->
                        GameDataStore.clear(this@LauncherActivity)
                        refreshStatus()
                    }
                    .setNegativeButton(R.string.cancel, null)
                    .show()
            }
        })

        content.addView(TextView(this).apply {
            setText(R.string.licence_notice)
            textSize = 11f
            setPadding(0, 32, 0, 0)
        })

        return ScrollView(this).apply { addView(content) }
    }

    private fun sectionLabel(resId: Int) = TextView(this).apply {
        setText(resId)
        textSize = 16f
        setTypeface(typeface, android.graphics.Typeface.BOLD)
        setPadding(0, 16, 0, 8)
    }

    private fun hint(resId: Int) = TextView(this).apply {
        setText(resId)
        textSize = 11f
        alpha = 0.7f
        setPadding(0, 0, 0, 12)
    }

    private fun divider() = View(this).apply {
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 2).apply {
            topMargin = 32
            bottomMargin = 16
        }
        setBackgroundColor(0x33FFFFFF)
    }

    // --- data ----------------------------------------------------------------

    private fun refreshStatus() {
        val status = GameDataStore.status(this)
        playButton.isEnabled = status.ready

        statusText.text = if (status.ready) {
            getString(
                R.string.data_ready,
                status.language ?: "english",
                Formatter.formatShortFileSize(this, status.totalBytes),
            )
        } else {
            getString(R.string.data_missing, status.missing.joinToString("\n  \u2022 ", "\n  \u2022 "))
        }

        importButton.setText(if (status.ready) R.string.reimport_folder else R.string.choose_folder)
    }

    private fun startImport(uri: Uri) {
        cancelImport = false
        importButton.isEnabled = false
        playButton.isEnabled = false
        progressBar.visibility = View.VISIBLE
        progressBar.progress = 0
        progressText.visibility = View.VISIBLE
        progressText.setText(R.string.scanning)

        // A full install is tens of gigabytes; this takes minutes and must
        // not touch the main thread.
        thread(name = "KisakCOD import") {
            var lastUpdateMs = 0L
            val result = GameDataStore.import(
                context = this,
                treeUri = uri,
                onProgress = { copied, total, file ->
                    val now = System.currentTimeMillis()
                    // Throttled: posting per buffer would flood the main
                    // thread's queue and make the bar lag behind the copy.
                    if (now - lastUpdateMs > 100L) {
                        lastUpdateMs = now
                        val fraction = if (total > 0) (copied * 1000 / total).toInt() else 0
                        runOnUiThread {
                            progressBar.progress = fraction
                            progressText.text = getString(
                                R.string.copying,
                                file,
                                Formatter.formatShortFileSize(this, copied),
                                Formatter.formatShortFileSize(this, total),
                            )
                        }
                    }
                },
                shouldCancel = { cancelImport },
            )

            runOnUiThread {
                progressBar.visibility = View.GONE
                progressText.visibility = View.GONE
                importButton.isEnabled = true
                result.fold(
                    onSuccess = { refreshStatus() },
                    onFailure = { error ->
                        refreshStatus()
                        Toast.makeText(
                            this,
                            error.message ?: getString(R.string.import_failed),
                            Toast.LENGTH_LONG,
                        ).show()
                    },
                )
            }
        }
    }

    private fun launchGame() {
        startActivity(Intent(this, GameActivity::class.java))
    }
}
