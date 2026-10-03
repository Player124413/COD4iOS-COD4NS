// Name bridge between the shared port layer and the Android platform layer.
//
// Four files in ports/ios turned out to be plain portable C++ with no Apple
// dependency at all, so the Android build compiles them directly rather than
// keeping a second copy that would drift:
//
//   ports/ios/d3d9/d3d9_apple.cpp        the Direct3D 9 object model
//   ports/ios/engine/controller_input.cpp  gamepad to usercmd mapping
//   ports/ios/engine/controller_icons.cpp  button glyphs in the HUD
//   ports/ios/engine/db_zoneload_apple.cpp fastfile asset registration
//   ports/ios/engine/cinematic_apple.cpp   cutscene playback
//
// They refer to a handful of host services by their Apple names. Rather than
// rename them in shared code - which would mean touching the iOS port to add
// Android - this file forwards each one to its Android equivalent. The cost
// is four function calls that the linker inlines away; the benefit is that a
// fix to the controller mapping lands on both platforms at once.
//
// ControllerFrame is not forwarded here. The Android side has real work to
// do first - merging the gamepad with the on-screen controls and publishing
// the result - so KisakAndroid_ControllerFrame lives in android_input.cpp
// and calls the shared KisakApple_ControllerFrame at the end.

#include <cstdint>

// Declared in ports/ios/d3d9/d3d9_apple.cpp and controller_input.cpp. HWND is
// the compat typedef from ports/ios/compat/native/windows; declaring the
// return type as void* here and casting avoids pulling the whole Win32
// compatibility surface into this file.
extern "C++" {

// --- implemented in ports/android/engine/android_sys.cpp ---
void *KisakAndroid_GetNativeWindow();
bool KisakAndroid_GetDisplaySize(int *width, int *height);
void KisakAndroid_ControllerCursor(float dx, float dy, int button);

} // extern "C++"

// The renderer asks for a window handle to hand to gpu::Initialize. On iOS
// that is a CAMetalLayer; here it is the ANativeWindow from the activity's
// SurfaceView, which vkCreateAndroidSurfaceKHR takes directly.
//
// d3d9_apple.cpp declares this as returning HWND, which the compatibility
// headers define as `typedef void* HANDLE; typedef HANDLE HWND;` - the same
// type, so the definition below matches the declaration exactly rather than
// relying on C++ ignoring return types when it mangles names.
void *KisakApple_GetRenderWindow()
{
    return KisakAndroid_GetNativeWindow();
}

bool KisakApple_GetDisplaySize(int *width, int *height)
{
    return KisakAndroid_GetDisplaySize(width, height);
}

void KisakApple_ControllerCursor(float dx, float dy, int button)
{
    KisakAndroid_ControllerCursor(dx, dy, button);
}
