#pragma once

#include <cstdint>

// Services the Android app layer provides to the engine, and the few engine
// hooks the app layer needs. Plain declarations so both the C++ engine (built
// with the Windows prelude) and the JNI glue can include this header.

// ---------------------------------------------------------------------------
// Events (ports/android/platform/android_events.cpp)

// Release an event only after all users have stopped accessing it. The Windows
// backend keeps its events until process exit; Android also needs explicit
// teardown when the activity is destroyed and the engine unloaded.
void Sys_DestroyAndroidEvent(void **event);

// ---------------------------------------------------------------------------
// Paths (ports/android/platform/android_storage.cpp)

// Where the game data lives. On Android this is the app's external files
// directory (/sdcard/Android/data/<package>/files), handed down from Java at
// startup because the NDK cannot derive it.
void KisakAndroid_SetStorageRoots(const char *gameData, const char *internalCache, const char *externalCache);
const char *KisakAndroid_GameDataPath();
const char *KisakAndroid_CachePath();
// Writable location for the shader/pipeline cache, saves and the log.
const char *KisakAndroid_PrivatePath();

// ---------------------------------------------------------------------------
// Surface and display (ports/android/app)

// Called from the UI thread when the ANativeWindow appears, changes size or
// goes away. A null window means the activity is backgrounded; the renderer
// tears down its swapchain and the engine keeps simulating.
void KisakAndroid_SetNativeWindow(void *window, int width, int height);
void *KisakAndroid_GetNativeWindow();
bool KisakAndroid_GetDisplaySize(int *width, int *height);
void KisakAndroid_SetDisplayRefreshRate(double hz);
void KisakAndroid_SetDisplaySafeArea(float horizontal, float vertical);
void KisakAndroid_GetDisplaySafeArea(float *horizontal, float *vertical);

// ---------------------------------------------------------------------------
// Lifecycle

// The activity was paused/resumed. The engine keeps running but stops
// presenting, releases the audio device and drops to a 10 Hz tick so a
// backgrounded game neither drains the battery nor gets killed for CPU use.
void KisakAndroid_SetForeground(bool foreground);
bool KisakAndroid_IsForeground();

// Starts the engine on its own thread. Returns false when it is already
// running. `commandLine` is appended to the engine's own.
bool KisakAndroid_StartEngine(const char *commandLine);
int KisakAndroid_RunEngine(const char *commandLine);

// ---------------------------------------------------------------------------
// Text input (the soft keyboard is a Java-side view)

void KisakAndroid_TextInput(const char *utf8);
void KisakAndroid_TextBackspace();
void KisakAndroid_TextReturn();
int KisakAndroid_TextInputActive();
// Set by the Java layer once the IME has actually opened or closed.
void KisakAndroid_SetSoftKeyboardVisible(int visible);

// ---------------------------------------------------------------------------
// Touch (ports/android/app/touch_controls.cpp)

// phase: 0 began, 1 moved, 2 ended, 3 cancelled. Coordinates in pixels.
void KisakAndroid_TouchEvent(int pointerId, int phase, float x, float y);

// ---------------------------------------------------------------------------
// Engine mode (ports/android/platform/android_engine_mode.cpp)

extern "C" {
// "sp" or "mp": which engine library the launcher loads next start.
void KisakAndroid_SetEngineMode(const char *mode);
const char *KisakAndroid_GetEngineMode();
void KisakAndroid_PromptEngineRestart(const char *mode);
}

// ---------------------------------------------------------------------------
// Thermal and power signals, pushed from Java (PowerManager has no NDK API
// below Android 11, and the listener form has none at all).

void KisakAndroid_OnThermalStatus(int status);
void KisakAndroid_OnThermalHeadroom(float headroom);
// Battery saver changed. The engine drops its frame rate target rather than
// fighting the governor for clocks it will not get.
void KisakAndroid_OnPowerSaveMode(bool enabled);
