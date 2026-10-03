// Gamepad state, and the per-frame merge that actually feeds the engine.
//
// The shared controller code (ports/ios/engine/controller_input.cpp) consumes
// one kisak::controller::Snapshot per frame and turns it into usercmd motion,
// menu navigation and button edges. On iOS the snapshot is assembled by the
// GameController framework callback in engine_app.mm. Android has no
// equivalent framework: MotionEvents and KeyEvents arrive on the UI thread
// from whatever InputDevice the player plugged in, so the state is
// accumulated here and sampled by the engine thread.
//
// Two sources are merged:
//
//   a physical gamepad, when one is connected
//   the on-screen controls (ports/android/app/touch_controls.cpp)
//
// The gamepad wins when it is present, and the on-screen controls hide
// themselves - a player holding a controller does not want half the screen
// covered in buttons. They are not summed, because a resting thumb on the
// stick region would then fight the gamepad's centred stick.
//
// Everything here is written from the UI thread and read from the engine
// thread, so the state is a single atomically-published struct rather than a
// set of independent atomics: a frame that saw the new buttons but the old
// stick position would aim in the wrong direction for one frame.

#include "../app/touch_controls.h"
#include "../platform/android_platform.h"
#include "controller_input.h"

#if defined(__ANDROID__)
#include <android/log.h>
#else
// Host builds (ports/android/tests) compile this file to exercise the merge
// logic; there is no liblog to link against there.
#define ANDROID_LOG_INFO 4
#define __android_log_print(...) ((void)0)
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

using kisak::controller::Snapshot;
namespace button = kisak::controller;

namespace {

struct GamepadState
{
    bool connected = false;
    uint32_t buttons = 0;
    float leftX = 0.0f, leftY = 0.0f;
    float rightX = 0.0f, rightY = 0.0f;
    float leftTrigger = 0.0f, rightTrigger = 0.0f;
};

std::mutex g_mutex;
GamepadState g_gamepad;

// Xbox-style labels. Android exposes no equivalent of
// GCControllerButtonInput.localizedName, so there is nothing to ask: the
// KEYCODE_BUTTON_A constant is a position on the pad, not a printed letter.
// Xbox layout is what the overwhelming majority of Android controllers are
// marked with, and it matches what the HUD glyphs in controller_icons.cpp
// were drawn for.
const char *const kLabels[button::Count] = {
    "A",     // South
    "B",     // East
    "X",     // West
    "Y",     // North
    "LB",    // L1
    "RB",    // R1
    "LT",    // L2
    "RT",    // R2
    "LS",    // L3
    "RS",    // R3
    "Menu",  // Menu
    "View",  // Options
    "Up", "Down", "Left", "Right",
};

// The table is indexed by bit position, so a button added to the shared enum
// without a label here would read off the end.
static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == button::Count,
              "label table must cover every kisak::controller::Button");

void FillLabels(Snapshot &snapshot)
{
    for (int index = 0; index < button::Count; ++index)
    {
        // The engine's font and localisation path is Windows-1252; these are
        // all ASCII, so the copy is the conversion.
        std::snprintf(snapshot.labels[index], sizeof(snapshot.labels[index]), "%s", kLabels[index]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Fed from ports/android/app/jni_bridge.cpp on the UI thread.

void KisakAndroid_GamepadConnected(bool connected)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_gamepad.connected == connected)
        return;
    // Reset on both edges. A controller unplugged mid-sprint would otherwise
    // leave the stick deflected and the player running into a wall; one
    // plugged in mid-game must not inherit stale axes.
    g_gamepad = GamepadState{};
    g_gamepad.connected = connected;
    __android_log_print(ANDROID_LOG_INFO, "KisakCOD", "gamepad %s",
                        connected ? "connected" : "disconnected");
}

void KisakAndroid_GamepadButton(int bit, bool pressed)
{
    if (bit < 0 || bit >= button::Count)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gamepad.connected = true;
    if (pressed)
        g_gamepad.buttons |= 1u << bit;
    else
        g_gamepad.buttons &= ~(1u << bit);
}

void KisakAndroid_GamepadAxes(float leftX, float leftY, float rightX, float rightY, float leftTrigger,
                              float rightTrigger)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gamepad.connected = true;
    // Android's Y axis grows downwards; the engine, like every other
    // consumer of this snapshot, expects up-positive.
    g_gamepad.leftX = std::clamp(leftX, -1.0f, 1.0f);
    g_gamepad.leftY = std::clamp(-leftY, -1.0f, 1.0f);
    g_gamepad.rightX = std::clamp(rightX, -1.0f, 1.0f);
    g_gamepad.rightY = std::clamp(-rightY, -1.0f, 1.0f);
    g_gamepad.leftTrigger = std::clamp(leftTrigger, 0.0f, 1.0f);
    g_gamepad.rightTrigger = std::clamp(rightTrigger, 0.0f, 1.0f);
}

bool KisakAndroid_GamepadPresent()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gamepad.connected;
}

// ---------------------------------------------------------------------------
// Called once per frame from IN_Frame, on the engine thread.

void KisakAndroid_ControllerFrame()
{
    GamepadState gamepad;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        gamepad = g_gamepad;
    }

    // Which input context the engine is in: 0 menus, 1 gameplay, 2 cinematic.
    // The on-screen controls need it to decide which buttons to show, and it
    // is the engine that knows.
    const int context = KisakApple_ControllerTouchContext();
    kisak::android::touch::SetContext(!gamepad.connected, context);

    Snapshot snapshot;
    if (gamepad.connected)
    {
        snapshot.connected = true;
        snapshot.touch = false;
        snapshot.buttons = gamepad.buttons;
        snapshot.leftX = gamepad.leftX;
        snapshot.leftY = gamepad.leftY;
        snapshot.rightX = gamepad.rightX;
        snapshot.rightY = gamepad.rightY;
        snapshot.leftTrigger = gamepad.leftTrigger;
        snapshot.rightTrigger = gamepad.rightTrigger;

        // Analogue triggers also report as buttons, so a pad that only sends
        // the axis still fires. The threshold has hysteresis in
        // TriggerPressed; here it only needs to be a sane default.
        if (gamepad.leftTrigger > 0.5f)
            snapshot.buttons |= 1u << button::L2;
        if (gamepad.rightTrigger > 0.5f)
            snapshot.buttons |= 1u << button::R2;
    }
    else
    {
        // The on-screen controls produce a Snapshot of the same shape, with
        // `touch` set so the shared code knows to apply swipe-look rather
        // than stick-look and to skip the stick deadzone, which a finger
        // does not need.
        snapshot = kisak::android::touch::Sample();
    }

    FillLabels(snapshot);
    KisakApple_ControllerSubmit(snapshot);

    // Turns the snapshot into usercmd motion, menu edges and cursor moves.
    // Must come after the submit: it reads what was just published.
    KisakApple_ControllerFrame();
}
