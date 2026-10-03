package ovh.kisak.cod4

import android.view.Surface

/**
 * The only class that talks to native code.
 *
 * Single player and multiplayer are separate shared libraries because the
 * `KISAK_MP` define changes structure layouts throughout the engine - the same
 * translation unit compiled both ways produces objects that cannot be linked
 * together. Which one is loaded is therefore decided once, at process start,
 * and changing it needs a process restart rather than a mode switch.
 *
 * Every method below is implemented in ports/android/app/jni_bridge.cpp. The
 * names are load-bearing: JNI resolves them by mangled name, so renaming one
 * here without renaming it there fails at first call rather than at build
 * time.
 */
object EngineBridge {

    enum class Mode(val libraryName: String, val id: String) {
        SinglePlayer("kisakcod_sp", "sp"),
        Multiplayer("kisakcod_mp", "mp");

        companion object {
            fun fromId(id: String?): Mode = if (id == "sp") SinglePlayer else Multiplayer
        }
    }

    @Volatile
    var loadedMode: Mode? = null
        private set

    @Volatile
    var loadError: String? = null
        private set

    /**
     * Loads one of the two engine libraries. Returns false and records the
     * reason in [loadError] rather than throwing, because a missing library is
     * something the launcher should explain rather than crash on.
     */
    @Synchronized
    fun load(mode: Mode): Boolean {
        loadedMode?.let { return it == mode }
        return try {
            System.loadLibrary(mode.libraryName)
            loadedMode = mode
            loadError = null
            true
        } catch (error: UnsatisfiedLinkError) {
            loadError = error.message ?: "could not load lib${mode.libraryName}.so"
            false
        }
    }

    // --- lifecycle -----------------------------------------------------------

    /** Caches a reference to the activity so native code can call back into it. */
    external fun nativeAttach(activity: GameActivity)
    external fun nativeDetach()

    external fun nativeSetStorageRoots(gameData: String, internalCache: String, externalCache: String)

    /**
     * Starts the engine thread. Returns false if the engine refused to start,
     * which in practice means the game data is missing or unreadable.
     */
    external fun nativeStartEngine(commandLine: String): Boolean

    external fun nativeSetForeground(foreground: Boolean)

    // --- surface -------------------------------------------------------------

    external fun nativeSurfaceCreated(surface: Surface)
    external fun nativeSurfaceChanged(surface: Surface, width: Int, height: Int)
    external fun nativeSurfaceDestroyed()

    external fun nativeSetDisplayMetrics(
        width: Int,
        height: Int,
        refreshHz: Float,
        safeHorizontal: Float,
        safeVertical: Float,
    )

    // --- input ---------------------------------------------------------------

    /** phase: 0 down, 1 move, 2 up, 3 cancel. */
    external fun nativeTouch(pointerId: Int, phase: Int, x: Float, y: Float)

    external fun nativeSetTouchControlsEnabled(enabled: Boolean)
    external fun nativeTouchControlsVisible(): Boolean

    /**
     * Fills [out] with `[x, y, w, h, isStick]` for each visible control and
     * returns how many were written. The layout comes from native code so the
     * rectangles that are drawn and the ones that are hit-tested cannot
     * disagree.
     */
    external fun nativeTouchControlLayout(out: FloatArray): Int

    external fun nativeTouchControlLabels(): Array<String>

    external fun nativeTextInput(text: String)
    external fun nativeTextBackspace()
    external fun nativeTextReturn()
    external fun nativeTextInputActive(): Boolean

    // --- settings ------------------------------------------------------------

    external fun nativeSetEngineMode(mode: String)
    external fun nativeGetEngineMode(): String

    external fun nativeSetDynamicResolution(enabled: Boolean)
    external fun nativeSetRenderScale(scale: Float)
    external fun nativeSetTargetFps(fps: Int)

    // --- thermal and power ---------------------------------------------------

    external fun nativeThermalStatus(status: Int)
    external fun nativeThermalHeadroom(headroom: Float)
    external fun nativePowerSaveMode(enabled: Boolean)

    // --- telemetry -----------------------------------------------------------

    /**
     * `[fps, cpuMs, gpuMs, p95FrameMs, missRatio, renderScale, targetFps,
     * thermalStatus]`, packed into one array because this is polled several
     * times a second.
     */
    external fun nativePerfStats(): FloatArray

    external fun nativePerfStatusLine(): String
}
