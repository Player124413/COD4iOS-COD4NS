package ovh.kisak.cod4

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.util.Log
import android.view.Choreographer
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * Hosts the engine.
 *
 * The structure is dictated by two things. First, the engine owns its own
 * thread with its own frame loop (ports/android/engine/android_sys.cpp) and
 * must not be driven from the UI thread - a loading screen that blocks for
 * eight seconds would otherwise trip the ANR watchdog. Second, the surface
 * can disappear and come back at any time, so the renderer has to survive
 * losing it rather than tear down the device.
 *
 * The choreographer callback here does not drive rendering. It only refreshes
 * the touch overlay and polls thermal state; the engine paces itself against
 * its own clock so that a frame is still produced while the activity is
 * mid-rotation and no callback is arriving.
 */
class GameActivity : AppCompatActivity(), SurfaceHolder.Callback, Choreographer.FrameCallback {

    private lateinit var settings: Settings
    private lateinit var surfaceView: SurfaceView
    private lateinit var overlay: TouchOverlayView
    private lateinit var perfText: TextView

    private var engineStarted = false
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null
    private var deviceListener: InputManager.InputDeviceListener? = null
    private var lastHeadroomPollMs = 0L
    private var frameCounter = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)

        if (!EngineBridge.load(settings.engineMode)) {
            showFatalError("The engine could not be loaded.\n\n${EngineBridge.loadError}")
            return
        }
        EngineBridge.nativeAttach(this)

        // Landscape only. The HUD, the touch layout and every menu in the
        // game assume a wide aspect; portrait would be a different port.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE

        // The screen must not sleep during a cutscene, where there is no
        // touch input for minutes at a time.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        goFullscreen()

        val root = FrameLayout(this)
        surfaceView = SurfaceView(this).apply {
            holder.addCallback(this@GameActivity)
        }
        overlay = TouchOverlayView(this)
        perfText = TextView(this).apply {
            setTextColor(0xFF9FE870.toInt())
            textSize = 11f
            setPadding(24, 24, 24, 24)
            visibility = if (settings.showPerfOverlay) View.VISIBLE else View.GONE
        }

        root.addView(surfaceView, FrameLayout.LayoutParams(MATCH, MATCH))
        root.addView(overlay, FrameLayout.LayoutParams(MATCH, MATCH))
        root.addView(
            perfText,
            FrameLayout.LayoutParams(WRAP, WRAP, Gravity.TOP or Gravity.END)
        )
        setContentView(root)

        // Rotated here and nowhere else. The log from a run that crashed has
        // to still be there when the player comes back to the launcher to
        // read it, so only the start of the next run may move it aside.
        GameLog.rotate(this)
        EngineBridge.nativeSetLogPath(GameLog.current(this).absolutePath)

        EngineBridge.nativeSetStorageRoots(
            GameDataStore.root(this).absolutePath,
            cacheDir.absolutePath,
            (externalCacheDir ?: cacheDir).absolutePath,
        )
        settings.applyLive()
        registerThermalListener()
        registerGamepadListener()
    }

    // --- surface -------------------------------------------------------------

    override fun surfaceCreated(holder: SurfaceHolder) {
        EngineBridge.nativeSurfaceCreated(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        publishDisplayMetrics(width, height)
        EngineBridge.nativeSurfaceChanged(holder.surface, width, height)

        // The engine is started on the first good surface rather than in
        // onCreate: the renderer needs a window to create its swapchain, and
        // starting earlier would mean a headless first second.
        if (!engineStarted) {
            engineStarted = EngineBridge.nativeStartEngine(buildCommandLine())
            if (!engineStarted) {
                showFatalError(
                    "The engine did not start. The game data in app storage is probably " +
                        "incomplete - reimport it from the launcher."
                )
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Returns only once the renderer has stopped using the window, so the
        // system is free to release it when this returns.
        EngineBridge.nativeSurfaceDestroyed()
    }

    private fun publishDisplayMetrics(width: Int, height: Int) {
        val refresh = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.refreshRate ?: 60f
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.refreshRate
        }

        // Keep the controls clear of the display cutout and the gesture bar.
        val insets = WindowCompat.getInsetsController(window, window.decorView)
        @Suppress("UNUSED_VARIABLE") val unused = insets
        val rootInsets = window.decorView.rootWindowInsets
        var safeHorizontal = 0f
        var safeVertical = 0f
        if (rootInsets != null) {
            val compat = WindowInsetsCompat.toWindowInsetsCompat(rootInsets)
            val bars = compat.getInsets(
                WindowInsetsCompat.Type.systemGestures() or WindowInsetsCompat.Type.displayCutout()
            )
            safeHorizontal = maxOf(bars.left, bars.right).toFloat()
            safeVertical = maxOf(bars.top, bars.bottom).toFloat()
        }

        EngineBridge.nativeSetDisplayMetrics(width, height, refresh, safeHorizontal, safeVertical)
    }

    private fun buildCommandLine(): String {
        val builder = StringBuilder()
        builder.append("+set fs_basepath \"").append(GameDataStore.root(this).absolutePath).append("\" ")
        GameDataStore.status(this).language?.let {
            builder.append("+set loc_language ").append(languageIndex(it)).append(' ')
        }
        builder.append(settings.extraCommandLine)
        return builder.toString().trim()
    }

    /** The engine names languages by index; english is 0 and the common default. */
    private fun languageIndex(language: String): Int = when (language.lowercase()) {
        "english" -> 0
        "french" -> 1
        "german" -> 2
        "italian" -> 3
        "spanish" -> 4
        "russian" -> 5
        "polish" -> 6
        "portuguese" -> 7
        "japanese" -> 8
        "korean" -> 9
        else -> 0
    }

    // --- lifecycle -----------------------------------------------------------

    override fun onResume() {
        super.onResume()
        goFullscreen()
        EngineBridge.nativeSetForeground(true)
        settings.applyLive()
        refreshGamepadState()
        Choreographer.getInstance().postFrameCallback(this)
    }

    override fun onPause() {
        super.onPause()
        // The engine keeps running at a slow tick rather than stopping: a
        // hard stop mid-level would lose the frame in flight, and Android
        // gives an app a few seconds after pause anyway. The native side
        // writes the pipeline cache here, because an app swiped out of
        // recents never reaches onDestroy.
        EngineBridge.nativeSetForeground(false)
        Choreographer.getInstance().removeFrameCallback(this)
    }

    override fun onDestroy() {
        deviceListener?.let {
            (getSystemService(Context.INPUT_SERVICE) as InputManager).unregisterInputDeviceListener(it)
        }
        deviceListener = null
        unregisterThermalListener()
        if (EngineBridge.loadedMode != null) {
            EngineBridge.nativeDetach()
        }
        super.onDestroy()
    }

    override fun doFrame(frameTimeNanos: Long) {
        overlay.refresh()

        if (settings.showPerfOverlay && ++frameCounter % 15 == 0) {
            perfText.text = runCatching { EngineBridge.nativePerfStatusLine() }.getOrDefault("")
        }

        // Thermal headroom is a prediction, not a measurement, and the
        // platform documents it as expensive: it is rate-limited to once a
        // second internally and polling faster just returns a cached value.
        val now = System.currentTimeMillis()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && now - lastHeadroomPollMs > 1000L) {
            lastHeadroomPollMs = now
            val power = getSystemService(Context.POWER_SERVICE) as PowerManager
            runCatching { power.getThermalHeadroom(10) }
                .getOrNull()
                ?.takeIf { !it.isNaN() }
                ?.let { EngineBridge.nativeThermalHeadroom(it) }
        }

        Choreographer.getInstance().postFrameCallback(this)
    }

    private fun registerThermalListener() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        val listener = PowerManager.OnThermalStatusChangedListener { status ->
            EngineBridge.nativeThermalStatus(status)
        }
        thermalListener = listener
        power.addThermalStatusListener(listener)
        EngineBridge.nativeThermalStatus(power.currentThermalStatus)
        EngineBridge.nativePowerSaveMode(power.isPowerSaveMode)
    }

    private fun unregisterThermalListener() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        thermalListener?.let {
            (getSystemService(Context.POWER_SERVICE) as PowerManager).removeThermalStatusListener(it)
        }
        thermalListener = null
    }

    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
    }

    // --- input ---------------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!engineStarted) return super.onTouchEvent(event)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val index = event.actionIndex
                EngineBridge.nativeTouch(
                    event.getPointerId(index), 0, event.getX(index), event.getY(index)
                )
            }

            MotionEvent.ACTION_MOVE -> {
                // Every pointer moves in one event, and historical samples
                // carry the positions between frames. Feeding them matters
                // for look sensitivity: dropping them makes a fast swipe
                // register as one long jump instead of a smooth arc.
                for (pointer in 0 until event.pointerCount) {
                    val id = event.getPointerId(pointer)
                    for (sample in 0 until event.historySize) {
                        EngineBridge.nativeTouch(
                            id, 1,
                            event.getHistoricalX(pointer, sample),
                            event.getHistoricalY(pointer, sample),
                        )
                    }
                    EngineBridge.nativeTouch(id, 1, event.getX(pointer), event.getY(pointer))
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val index = event.actionIndex
                EngineBridge.nativeTouch(
                    event.getPointerId(index), 2, event.getX(index), event.getY(index)
                )
            }

            MotionEvent.ACTION_CANCEL -> {
                for (pointer in 0 until event.pointerCount) {
                    EngineBridge.nativeTouch(event.getPointerId(pointer), 3, 0f, 0f)
                }
            }
        }
        return true
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // A controller's own Back button must not leave the game, so the
        // gamepad path is checked before the Back handling below.
        if (GamepadInput.onKey(event)) return true

        if (keyCode == KeyEvent.KEYCODE_BACK) {
            // Back opens the pause menu rather than leaving the game. Quitting
            // mid-level by accident is the single worst thing a gesture can do
            // in a game with no quicksave on this platform.
            EngineBridge.nativeTextInput("\u001b")
            return true
        }
        if (EngineBridge.nativeTextInputActive() && event.unicodeChar != 0) {
            EngineBridge.nativeTextInput(String(Character.toChars(event.unicodeChar)))
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (GamepadInput.onKey(event)) return true
        return super.onKeyUp(keyCode, event)
    }

    // Sticks, triggers and the D-pad hat all arrive here rather than as key
    // events. Without this override a controller can press buttons but not
    // move or aim.
    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (GamepadInput.onMotion(event)) return true
        return super.onGenericMotionEvent(event)
    }

    private fun registerGamepadListener() {
        val manager = getSystemService(Context.INPUT_SERVICE) as InputManager
        val listener = GamepadInput.createListener { refreshGamepadState() }
        deviceListener = listener
        manager.registerInputDeviceListener(listener, null)
        refreshGamepadState()
    }

    private fun refreshGamepadState() {
        val connected = GamepadInput.anyConnected()
        if (!connected) GamepadInput.reset()
        EngineBridge.nativeGamepadConnected(connected)
        // The on-screen controls hide themselves while a controller is
        // attached; the native side already knows, but the setting is what
        // decides whether they may appear at all.
        EngineBridge.nativeSetTouchControlsEnabled(settings.touchControls && !connected)
    }

    // --- called from native (ports/android/app/jni_bridge.cpp) ---------------
    // Names and signatures are resolved by GetMethodID at attach time, so a
    // rename here has to be matched there.

    fun showFatalError(message: String) {
        runOnUiThread {
            if (isFinishing || isDestroyed) return@runOnUiThread
            AlertDialog.Builder(this)
                .setTitle(R.string.fatal_title)
                .setMessage(message)
                .setCancelable(false)
                // The dialog is the last thing the player sees of this run,
                // and the engine thread is already gone. Offering the log
                // here saves them finding their way back to the launcher to
                // fetch something they have just been told they need.
                .setNeutralButton(R.string.log_copy) { _, _ ->
                    GameLog.copyToClipboard(this, GameLog.read(this))
                    Toast.makeText(this, R.string.log_copied, Toast.LENGTH_LONG).show()
                    finishAndRemoveTask()
                }
                .setPositiveButton(R.string.quit) { _, _ -> finishAndRemoveTask() }
                .show()
        }
    }

    fun requestQuit() {
        runOnUiThread { finishAndRemoveTask() }
    }

    fun openUrl(url: String) {
        runOnUiThread {
            runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
                .onFailure { Log.w(TAG, "could not open $url", it) }
        }
    }

    fun getClipboardText(): String {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        return clipboard.primaryClip?.takeIf { it.itemCount > 0 }
            ?.getItemAt(0)?.coerceToText(this)?.toString()
            .orEmpty()
    }

    fun setClipboardText(text: String) {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("KisakCOD", text))
    }

    fun showRestartPrompt(mode: String) {
        runOnUiThread {
            if (isFinishing || isDestroyed) return@runOnUiThread
            AlertDialog.Builder(this)
                .setTitle(R.string.restart_title)
                // Single player and multiplayer are separate libraries, so
                // switching between them genuinely needs a new process.
                .setMessage(getString(R.string.restart_message, mode))
                .setPositiveButton(R.string.restart) { _, _ ->
                    settings.engineMode = EngineBridge.Mode.fromId(mode)
                    val intent = Intent(this, LauncherActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                    startActivity(intent)
                    // The engine's global state cannot be unwound, so the
                    // process is replaced rather than reused.
                    finishAndRemoveTask()
                    Runtime.getRuntime().exit(0)
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }
    }

    fun setSoftKeyboardVisible(visible: Boolean) {
        runOnUiThread {
            val manager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            if (visible) {
                surfaceView.requestFocus()
                manager.showSoftInput(surfaceView, InputMethodManager.SHOW_IMPLICIT)
            } else {
                manager.hideSoftInputFromWindow(surfaceView.windowToken, 0)
            }
        }
    }

    fun setPreferredFrameRate(hz: Float) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        runOnUiThread {
            runCatching {
                surfaceView.holder.surface.setFrameRate(
                    hz,
                    Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                )
            }
        }
    }

    private companion object {
        const val TAG = "KisakCOD"
        const val MATCH = FrameLayout.LayoutParams.MATCH_PARENT
        const val WRAP = FrameLayout.LayoutParams.WRAP_CONTENT
    }
}
