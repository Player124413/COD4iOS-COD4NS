package ovh.kisak.cod4

import android.hardware.input.InputManager
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Translates Android gamepad events into the engine's button and axis model.
 *
 * Android gives a controller to an app as raw KeyEvents and MotionEvents with
 * no notion of a standard layout, so the mapping has to be written out. The
 * bit indices below are positions in `kisak::controller::Button`
 * (ports/ios/engine/controller_input.h) and are shared with iOS; the engine
 * never sees an Android keycode.
 *
 * Two things here are not obvious:
 *
 * Triggers come through as axes, not buttons, on almost every controller -
 * and on three different axis pairs depending on the vendor. All three are
 * read and the largest taken, which is correct because a controller only
 * populates one of them.
 *
 * The D-pad is a hat axis on most controllers and KeyEvents on some. Both
 * are handled, and the hat path synthesises press and release edges itself,
 * because a hat returning to centre produces no KeyEvent at all.
 */
object GamepadInput {

    // Must match kisak::controller::Button.
    private const val SOUTH = 0
    private const val EAST = 1
    private const val WEST = 2
    private const val NORTH = 3
    private const val L1 = 4
    private const val R1 = 5
    private const val L2 = 6
    private const val R2 = 7
    private const val L3 = 8
    private const val R3 = 9
    private const val MENU = 10
    private const val OPTIONS = 11
    private const val UP = 12
    private const val DOWN = 13
    private const val LEFT = 14
    private const val RIGHT = 15

    /**
     * Sticks rest slightly off centre on worn hardware, and a controller left
     * on a table should not make the player drift. The shared code applies a
     * proper radial deadzone; this is only a floor to stop jitter crossing
     * JNI sixty times a second.
     */
    private const val AXIS_NOISE = 0.02f

    private var lastHatX = 0
    private var lastHatY = 0

    private fun keyToBit(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> SOUTH
        KeyEvent.KEYCODE_BUTTON_B -> EAST
        KeyEvent.KEYCODE_BUTTON_X -> WEST
        KeyEvent.KEYCODE_BUTTON_Y -> NORTH
        KeyEvent.KEYCODE_BUTTON_L1 -> L1
        KeyEvent.KEYCODE_BUTTON_R1 -> R1
        KeyEvent.KEYCODE_BUTTON_L2 -> L2
        KeyEvent.KEYCODE_BUTTON_R2 -> R2
        KeyEvent.KEYCODE_BUTTON_THUMBL -> L3
        KeyEvent.KEYCODE_BUTTON_THUMBR -> R3
        // Start and the newer "Menu" keycode both mean pause. Xbox controllers
        // send KEYCODE_BUTTON_START; some DualSense firmware sends MENU.
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> MENU
        // Back/Select/View is the scoreboard, matching the Xbox 360 binding
        // the engine's own defaults were written for.
        KeyEvent.KEYCODE_BUTTON_SELECT -> OPTIONS
        KeyEvent.KEYCODE_DPAD_UP -> UP
        KeyEvent.KEYCODE_DPAD_DOWN -> DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> RIGHT
        else -> -1
    }

    /** True if the event came from something that is actually a controller. */
    fun isGamepadSource(source: Int): Boolean =
        source and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            source and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK ||
            source and InputDevice.SOURCE_DPAD == InputDevice.SOURCE_DPAD

    /** Returns true when the event was consumed. */
    fun onKey(event: KeyEvent): Boolean {
        if (!isGamepadSource(event.source)) return false
        // A held button repeats; the engine wants edges, and a repeat would
        // read as a second press.
        if (event.repeatCount > 0) return true

        val bit = keyToBit(event.keyCode)
        if (bit < 0) return false

        EngineBridge.nativeGamepadButton(bit, event.action == KeyEvent.ACTION_DOWN)
        return true
    }

    /** Returns true when the event was consumed. */
    fun onMotion(event: MotionEvent): Boolean {
        if (!isGamepadSource(event.source) || event.action != MotionEvent.ACTION_MOVE) return false

        val leftX = axis(event, MotionEvent.AXIS_X)
        val leftY = axis(event, MotionEvent.AXIS_Y)

        // The right stick is Z/RZ on Xbox-style pads and RX/RY on a few
        // others. Reading both and taking whichever is non-zero covers the
        // whole field without a per-device table.
        val rightX = pick(axis(event, MotionEvent.AXIS_Z), axis(event, MotionEvent.AXIS_RX))
        val rightY = pick(axis(event, MotionEvent.AXIS_RZ), axis(event, MotionEvent.AXIS_RY))

        // Triggers: the documented pair, the brake/gas pair that several
        // vendors use instead, and LTRIGGER/RTRIGGER on older devices.
        val leftTrigger = maxOf(
            axis(event, MotionEvent.AXIS_LTRIGGER),
            axis(event, MotionEvent.AXIS_BRAKE),
        ).coerceIn(0f, 1f)
        val rightTrigger = maxOf(
            axis(event, MotionEvent.AXIS_RTRIGGER),
            axis(event, MotionEvent.AXIS_GAS),
        ).coerceIn(0f, 1f)

        EngineBridge.nativeGamepadAxes(leftX, leftY, rightX, rightY, leftTrigger, rightTrigger)

        // The D-pad as a hat. Edges are synthesised because returning to
        // centre sends no KeyEvent, and without this a direction sticks.
        val hatX = sign(axis(event, MotionEvent.AXIS_HAT_X))
        val hatY = sign(axis(event, MotionEvent.AXIS_HAT_Y))
        if (hatX != lastHatX) {
            EngineBridge.nativeGamepadButton(LEFT, hatX < 0)
            EngineBridge.nativeGamepadButton(RIGHT, hatX > 0)
            lastHatX = hatX
        }
        if (hatY != lastHatY) {
            // Hat Y is positive downwards, like every other Android axis.
            EngineBridge.nativeGamepadButton(UP, hatY < 0)
            EngineBridge.nativeGamepadButton(DOWN, hatY > 0)
            lastHatY = hatY
        }
        return true
    }

    private fun axis(event: MotionEvent, axis: Int): Float {
        val value = event.getAxisValue(axis)
        return if (abs(value) < AXIS_NOISE) 0f else value
    }

    private fun pick(primary: Float, fallback: Float): Float =
        if (primary != 0f) primary else fallback

    private fun sign(value: Float): Int = when {
        value > 0.5f -> 1
        value < -0.5f -> -1
        else -> 0
    }

    /** Any controller currently attached? Drives whether the overlay shows. */
    fun anyConnected(): Boolean = InputDevice.getDeviceIds().any { id ->
        val device = InputDevice.getDevice(id) ?: return@any false
        isGamepadSource(device.sources) && !device.isVirtual
    }

    /**
     * Watches for controllers being plugged in and out. Without this the
     * overlay would not come back when a controller's battery dies mid-match,
     * leaving the player with no controls at all.
     */
    fun createListener(onChanged: () -> Unit) = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = onChanged()
        override fun onInputDeviceRemoved(deviceId: Int) = onChanged()
        override fun onInputDeviceChanged(deviceId: Int) = onChanged()
    }

    fun reset() {
        lastHatX = 0
        lastHatY = 0
    }
}
