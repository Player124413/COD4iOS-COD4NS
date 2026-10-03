#pragma once

// Native side of the on-screen controls. The layout and gesture rules live in
// ports/ios/input/TouchControls.h and are shared with the iOS port; this
// header is the surface the JNI bridge and the Kotlin overlay talk to.

#include <cstddef>
#include <cstdint>

#include "../../ios/engine/controller_input.h"

namespace kisak::android::touch {

// Bounds of the drawable area in view pixels, inset by the display cutout and
// gesture insets the activity reports.
void SetBounds(float x, float y, float width, float height);

// context: 0 menus, 1 gameplay, 2 cinematic - the value the engine publishes
// through KisakApple_ControllerTouchContext.
void SetContext(bool available, int context);

// The player can turn the overlay off entirely when a gamepad is attached.
void SetEnabled(bool enabled);
bool Enabled();

void Down(int64_t pointerId, float x, float y);
void Move(int64_t pointerId, float x, float y);
void Up(int64_t pointerId);
void CancelPointer(int64_t pointerId);
void CancelAll();

// Sampled once per engine frame and merged with any physical gamepad.
kisak::controller::Snapshot Sample();

bool Visible();

// Layout query for the overlay, so the drawn buttons and the hit-tested ones
// are the same rectangles by construction.
size_t ControlCount();
bool ControlFrame(size_t index, float *x, float *y, float *width, float *height, const char **label,
                  bool *isStick);
bool StickKnob(float *x, float *y);

} // namespace kisak::android::touch
