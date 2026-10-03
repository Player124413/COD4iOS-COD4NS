package ovh.kisak.cod4

import android.content.Context
import android.content.SharedPreferences

/**
 * Launcher settings.
 *
 * These are the knobs the player sees. The performance subsystem picks
 * sensible values for the device on its own (ports/android/perf), so every
 * setting here is either an override of that or something it cannot know -
 * which hand the player holds the phone in, say.
 */
class Settings(context: Context) {

    private val preferences: SharedPreferences =
        context.getSharedPreferences("kisakcod", Context.MODE_PRIVATE)

    var engineMode: EngineBridge.Mode
        get() = EngineBridge.Mode.fromId(preferences.getString(KEY_MODE, "mp"))
        set(value) = preferences.edit().putString(KEY_MODE, value.id).apply()

    /**
     * When on, the renderer trades resolution for frame rate automatically.
     * On by default and recommended: it is what holds 60 fps on a mid-range
     * part through a firefight, and the alternative is dropped frames.
     */
    var dynamicResolution: Boolean
        get() = preferences.getBoolean(KEY_DYNAMIC_RESOLUTION, true)
        set(value) = preferences.edit().putBoolean(KEY_DYNAMIC_RESOLUTION, value).apply()

    /** Fixed scale used when [dynamicResolution] is off. 0.6 to 1.0. */
    var renderScale: Float
        get() = preferences.getFloat(KEY_RENDER_SCALE, 1.0f).coerceIn(0.6f, 1.0f)
        set(value) = preferences.edit().putFloat(KEY_RENDER_SCALE, value.coerceIn(0.6f, 1.0f)).apply()

    /**
     * 60 everywhere by default. A 120 Hz panel can run the engine faster, but
     * the animation and physics code was written against a 60 Hz assumption
     * and a sustained 120 costs roughly twice the power for a result most
     * players will not notice on a phone.
     */
    var targetFps: Int
        get() = preferences.getInt(KEY_TARGET_FPS, 60)
        set(value) = preferences.edit().putInt(KEY_TARGET_FPS, value.coerceIn(30, 144)).apply()

    var touchControls: Boolean
        get() = preferences.getBoolean(KEY_TOUCH_CONTROLS, true)
        set(value) = preferences.edit().putBoolean(KEY_TOUCH_CONTROLS, value).apply()

    /** Shows the frame time overlay. Off by default; useful in a bug report. */
    var showPerfOverlay: Boolean
        get() = preferences.getBoolean(KEY_PERF_OVERLAY, false)
        set(value) = preferences.edit().putBoolean(KEY_PERF_OVERLAY, value).apply()

    /** Extra arguments appended to the engine command line, for testing. */
    var extraCommandLine: String
        get() = preferences.getString(KEY_EXTRA_ARGS, "").orEmpty()
        set(value) = preferences.edit().putString(KEY_EXTRA_ARGS, value).apply()

    /** Pushes everything that can change at runtime into the engine. */
    fun applyLive() {
        EngineBridge.nativeSetDynamicResolution(dynamicResolution)
        EngineBridge.nativeSetRenderScale(renderScale)
        EngineBridge.nativeSetTargetFps(targetFps)
        EngineBridge.nativeSetTouchControlsEnabled(touchControls)
    }

    private companion object {
        const val KEY_MODE = "engine_mode"
        const val KEY_DYNAMIC_RESOLUTION = "dynamic_resolution"
        const val KEY_RENDER_SCALE = "render_scale"
        const val KEY_TARGET_FPS = "target_fps"
        const val KEY_TOUCH_CONTROLS = "touch_controls"
        const val KEY_PERF_OVERLAY = "perf_overlay"
        const val KEY_EXTRA_ARGS = "extra_args"
    }
}
