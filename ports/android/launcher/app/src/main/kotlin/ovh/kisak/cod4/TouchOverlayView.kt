package ovh.kisak.cod4

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.View

/**
 * Draws the on-screen controls.
 *
 * Only draws. The layout and all the gesture handling live in native code
 * (ports/android/app/touch_controls.cpp, over the state machine shared with
 * the iOS port), and this view asks for the rectangles each frame rather than
 * keeping its own copy - so what is drawn and what responds to a finger are
 * the same rectangles by construction.
 *
 * Drawing in a View rather than in the engine's Vulkan frame is deliberate:
 * the overlay changes a few times a second while the game runs at 60, so
 * letting the system composite a mostly-static layer costs less than redrawing
 * it 60 times into the game's own frame. It also means the controls stay
 * visible and responsive during a loading screen, when the engine is not
 * producing frames at all.
 */
class TouchOverlayView(context: Context) : View(context) {

    // Five floats per control: x, y, w, h, isStick.
    private val layout = FloatArray(MAX_CONTROLS * 5)
    private var controlCount = 0
    private var labels: Array<String> = emptyArray()

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(20, 255, 255, 255)
    }

    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
        color = Color.argb(82, 255, 255, 255)
    }

    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(235, 255, 255, 255)
        textAlign = Paint.Align.CENTER
        isFakeBoldText = true
    }

    private val rect = RectF()

    init {
        // The view never handles touches itself: the activity owns the whole
        // surface so a finger that starts on a button and slides off still
        // belongs to the same gesture.
        isClickable = false
        isFocusable = false
        setWillNotDraw(false)
        setLayerType(LAYER_TYPE_HARDWARE, null)
    }

    fun refresh() {
        if (labels.isEmpty() && EngineBridge.loadedMode != null) {
            labels = runCatching { EngineBridge.nativeTouchControlLabels() }.getOrDefault(emptyArray())
        }
        val count = runCatching { EngineBridge.nativeTouchControlLayout(layout) }.getOrDefault(0)
        if (count != controlCount || count > 0) {
            controlCount = count
            // invalidate() rather than postInvalidateOnAnimation(): this is
            // already called from the choreographer callback in GameActivity.
            invalidate()
        }
        if (count == 0 && visibility != GONE) {
            visibility = GONE
        } else if (count > 0 && visibility != VISIBLE) {
            visibility = VISIBLE
        }
    }

    override fun onDraw(canvas: Canvas) {
        for (index in 0 until controlCount) {
            val base = index * 5
            val x = layout[base]
            val y = layout[base + 1]
            val width = layout[base + 2]
            val height = layout[base + 3]
            val isStick = layout[base + 4] > 0.5f

            if (isStick) {
                val centreX = x + width * 0.5f
                val centreY = y + height * 0.5f
                val radius = minOf(width, height) * 0.5f
                canvas.drawCircle(centreX, centreY, radius, fillPaint)
                canvas.drawCircle(centreX, centreY, radius, strokePaint)
                // The knob position is not queried per frame: the finger is
                // already on it, so the player's own thumb is the feedback.
                canvas.drawCircle(centreX, centreY, radius * 0.32f, strokePaint)
                continue
            }

            rect.set(x, y, x + width, y + height)
            val corner = minOf(width, height) * 0.22f
            canvas.drawRoundRect(rect, corner, corner, fillPaint)
            canvas.drawRoundRect(rect, corner, corner, strokePaint)

            val label = labels.getOrNull(index).orEmpty()
            if (label.isNotEmpty()) {
                textPaint.textSize = (minOf(width, height) * 0.30f).coerceIn(18f, 44f)
                // Baseline, not centre: drawText places the baseline, so the
                // text sits low without this correction.
                val baseline = rect.centerY() - (textPaint.descent() + textPaint.ascent()) * 0.5f
                canvas.drawText(label, rect.centerX(), baseline, textPaint)
            }
        }
    }

    private companion object {
        const val MAX_CONTROLS = 24
    }
}
