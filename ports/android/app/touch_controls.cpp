// On-screen touch controls.
//
// The state machine itself is shared with the iOS port
// (ports/ios/input/TouchControls.h): the same layout, the same deadzones, the
// same stickiness rules for taps that become drags. Only the two ends differ.
// On iOS a UIView owns the touches and draws the buttons; here the Kotlin
// overlay draws them and MotionEvents arrive through JNI, because drawing in
// native code would mean a second Vulkan surface composited over the game for
// no gain.
//
// The Kotlin side asks this file where the controls are rather than carrying
// its own copy of the layout, so the thing that is drawn and the thing that
// is hit-tested can never disagree.

#include "touch_controls.h"

#include "../../ios/input/TouchControls.h"

#include <chrono>
#include <mutex>

namespace {

// The engine thread samples while the UI thread delivers touches, so every
// entry point takes this.
std::mutex g_mutex;
kisak::touch::State g_state;
bool g_enabled = true;

double NowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

} // namespace

namespace kisak::android::touch {

void SetBounds(float x, float y, float width, float height)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.SetBounds({ x, y, width, height });
}

void SetContext(bool available, int context)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // Context 0 is menus, 1 is gameplay, 2 is a cinematic. A cinematic shows
    // nothing: the only useful input is a tap to skip, which the activity
    // handles itself.
    const bool visible = g_enabled && available && context != 2;
    g_state.SetContext(visible, context == 1);
}

void SetEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_enabled = enabled;
    if (!enabled)
        g_state.Cancel();
}

bool Enabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled;
}

void Down(int64_t pointerId, float x, float y)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.Down(static_cast<uintptr_t>(pointerId), { x, y }, NowSeconds());
}

void Move(int64_t pointerId, float x, float y)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.Move(static_cast<uintptr_t>(pointerId), { x, y });
}

void Up(int64_t pointerId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.Up(static_cast<uintptr_t>(pointerId));
}

void CancelPointer(int64_t pointerId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // ACTION_CANCEL means the gesture was taken over by something else - a
    // system back swipe, usually. Releasing rather than ending the touch
    // stops a held fire button from sticking down after the gesture.
    g_state.CancelTouch(static_cast<uintptr_t>(pointerId));
}

void CancelAll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.Cancel();
}

kisak::controller::Snapshot Sample()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.Sample(NowSeconds());
}

bool Visible()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.Available();
}

size_t ControlCount()
{
    return kisak::touch::count;
}

bool ControlFrame(size_t index, float *x, float *y, float *width, float *height, const char **label,
                  bool *isStick)
{
    if (index >= kisak::touch::count)
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);

    // Hidden in menus unless this control does something in a menu.
    if (!g_state.Available())
        return false;
    if (!g_state.Gameplay() && !kisak::touch::MenuControl(index))
        return false;

    const kisak::touch::Rect frame = g_state.Frame(index);
    if (x) *x = frame.x;
    if (y) *y = frame.y;
    if (width) *width = frame.w;
    if (height) *height = frame.h;
    if (label) *label = kisak::touch::controls[index].label;
    // Control 0 is the movement stick, which the overlay draws as a ring plus
    // a knob rather than a labelled pad.
    if (isStick) *isStick = index == 0;
    return true;
}

bool StickKnob(float *x, float *y)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.Available() || !g_state.Gameplay())
        return false;
    const kisak::touch::Point offset = g_state.MoveKnob();
    const kisak::touch::Rect frame = g_state.Frame(0);
    const kisak::touch::Point center = frame.Center();
    if (x) *x = center.x + offset.x;
    if (y) *y = center.y + offset.y;
    return true;
}

} // namespace kisak::android::touch
