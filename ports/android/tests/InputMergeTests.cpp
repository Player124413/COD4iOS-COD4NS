// Host tests for the gamepad/touch merge in ports/android/engine/android_input.cpp.
//
// This is the one piece of new Android code with real branching in it, and
// the one that is hardest to debug on a device: a wrong sign or a swapped
// axis shows up as "aiming is inverted sometimes" rather than as a crash.
// The engine side is stubbed, so what is under test is exactly the merge.

#include "../../ios/engine/controller_input.h"
#include "../app/touch_controls.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace button = kisak::controller;

// --- stubs for the engine side ---------------------------------------------
static kisak::controller::Snapshot g_submitted;
static int g_submitCount = 0;
static int g_frameCount = 0;
static int g_context = 1;

void KisakApple_ControllerSubmit(const kisak::controller::Snapshot &snapshot)
{
    g_submitted = snapshot;
    ++g_submitCount;
}
void KisakApple_ControllerFrame() { ++g_frameCount; }
int KisakApple_ControllerTouchContext() { return g_context; }

// --- the unit under test ----------------------------------------------------
void KisakAndroid_GamepadConnected(bool connected);
void KisakAndroid_GamepadButton(int bit, bool pressed);
void KisakAndroid_GamepadAxes(float lx, float ly, float rx, float ry, float lt, float rt);
bool KisakAndroid_GamepadPresent();
void KisakAndroid_ControllerFrame();

// --- harness ----------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

static void Check(bool condition, const char *what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

static void CheckNear(float actual, float expected, const char *what)
{
    ++g_checks;
    if (std::fabs(actual - expected) > 1e-4f)
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %f, want %f)\n", what, actual, expected);
    }
}

int main()
{
    std::printf("android_input_merge\n");

    // A fresh process has no controller, so the touch path must be the one
    // that runs. Nothing should have reached the engine before a frame.
    Check(!KisakAndroid_GamepadPresent(), "no gamepad at startup");

    kisak::android::touch::SetEnabled(true);
    KisakAndroid_ControllerFrame();
    Check(g_submitCount == 1, "a frame submits exactly one snapshot");
    Check(g_frameCount == 1, "a frame drives the shared controller code once");
    Check(g_submitted.touch, "touch snapshot is flagged as touch");
    // `connected` means "this input source is live", NOT "a physical pad is
    // attached" - controller_input.cpp:261 drops the whole snapshot when it
    // is false, so a touch sample must set it too or the on-screen controls
    // would do nothing at all. `touch` is what distinguishes the two.
    Check(g_submitted.connected, "touch snapshot is live, not ignored");

    // Labels must be filled on every snapshot, touch included: the HUD asks
    // for them to draw "Press A" prompts and an empty string draws nothing.
    Check(std::strcmp(g_submitted.labels[button::South], "A") == 0, "South is labelled A");
    Check(std::strcmp(g_submitted.labels[button::R1], "RB") == 0, "R1 is labelled RB");
    Check(std::strcmp(g_submitted.labels[button::Right], "Right") == 0, "Right is labelled");
    for (int i = 0; i < button::Count; ++i)
        Check(g_submitted.labels[i][0] != '\0', "every button has a label");

    // Plugging in a pad takes over.
    KisakAndroid_GamepadConnected(true);
    Check(KisakAndroid_GamepadPresent(), "gamepad reported present");
    KisakAndroid_ControllerFrame();
    Check(g_submitted.connected, "gamepad snapshot is live");
    Check(!g_submitted.touch, "gamepad snapshot is not flagged as touch");

    // Android reports Y down-positive. Pushing the stick *up* gives -1 there
    // and must reach the engine as +1, or the player looks the wrong way.
    KisakAndroid_GamepadAxes(0.5f, -1.0f, -0.25f, 1.0f, 0.0f, 0.0f);
    KisakAndroid_ControllerFrame();
    CheckNear(g_submitted.leftX, 0.5f, "left X passes through");
    CheckNear(g_submitted.leftY, 1.0f, "left Y is flipped to up-positive");
    CheckNear(g_submitted.rightX, -0.25f, "right X passes through");
    CheckNear(g_submitted.rightY, -1.0f, "right Y is flipped");

    // Out-of-range values from a badly behaved driver must not escape.
    KisakAndroid_GamepadAxes(5.0f, 5.0f, -5.0f, -5.0f, 9.0f, -9.0f);
    KisakAndroid_ControllerFrame();
    CheckNear(g_submitted.leftX, 1.0f, "left X clamped");
    CheckNear(g_submitted.leftY, -1.0f, "left Y clamped");
    CheckNear(g_submitted.rightX, -1.0f, "right X clamped");
    CheckNear(g_submitted.rightY, 1.0f, "right Y clamped");
    CheckNear(g_submitted.leftTrigger, 1.0f, "left trigger clamped to 0..1");
    CheckNear(g_submitted.rightTrigger, 0.0f, "right trigger clamped to 0..1");
    Check((g_submitted.buttons & (1u << button::L2)) != 0, "full trigger presses L2");
    Check((g_submitted.buttons & (1u << button::R2)) == 0, "released trigger does not press R2");

    // Buttons are bit positions, and out-of-range indices are dropped rather
    // than corrupting a neighbouring bit.
    KisakAndroid_GamepadAxes(0, 0, 0, 0, 0, 0);
    KisakAndroid_GamepadButton(button::North, true);
    KisakAndroid_GamepadButton(99, true);
    KisakAndroid_GamepadButton(-1, true);
    KisakAndroid_ControllerFrame();
    Check(g_submitted.buttons == (1u << button::North), "only the named button is set");

    KisakAndroid_GamepadButton(button::North, false);
    KisakAndroid_ControllerFrame();
    Check(g_submitted.buttons == 0, "release clears the bit");

    // A pad unplugged mid-sprint must not leave the stick deflected; the
    // player would run into a wall until they found a controller.
    KisakAndroid_GamepadAxes(1.0f, 1.0f, 0, 0, 0, 0);
    KisakAndroid_GamepadButton(button::South, true);
    KisakAndroid_GamepadConnected(false);
    KisakAndroid_ControllerFrame();
    Check(g_submitted.touch, "disconnect falls back to the touch path");
    Check(g_submitted.connected, "the fallback snapshot is still live");
    Check(g_submitted.buttons == 0, "disconnect clears held buttons");
    KisakAndroid_GamepadConnected(true);
    KisakAndroid_ControllerFrame();
    CheckNear(g_submitted.leftX, 0.0f, "reconnect does not inherit a stale stick");
    CheckNear(g_submitted.leftY, 0.0f, "reconnect does not inherit stale Y");

    // The on-screen controls must be told to hide while a pad is attached,
    // and a cinematic shows none either way - the only useful input there is
    // a tap to skip, which the activity handles itself.
    Check(!kisak::android::touch::Visible(), "touch controls hide behind a gamepad");

    KisakAndroid_GamepadConnected(false);
    g_context = 1;
    KisakAndroid_ControllerFrame();
    Check(kisak::android::touch::Visible(), "touch controls return when the pad leaves");

    g_context = 2;
    KisakAndroid_ControllerFrame();
    Check(!kisak::android::touch::Visible(), "touch controls hide during a cinematic");

    g_context = 0;
    KisakAndroid_ControllerFrame();
    Check(kisak::android::touch::Visible(), "touch controls are shown in menus");

    // Turning the overlay off in settings outranks everything else.
    kisak::android::touch::SetEnabled(false);
    g_context = 1;
    KisakAndroid_ControllerFrame();
    Check(!kisak::android::touch::Visible(), "disabled overlay stays hidden");
    Check(g_submitCount == g_frameCount, "every submit is followed by a frame");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
