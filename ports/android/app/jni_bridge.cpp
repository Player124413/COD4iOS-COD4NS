// JNI bridge between the Kotlin activity and the engine.
//
// Everything that crosses the language boundary goes through this file, and
// nothing else in the port calls JNI. That is deliberate: JNI calls need a
// thread attached to the VM, and the engine thread is created with
// pthread_create and never attached. Centralising the boundary here means
// there is exactly one place that has to get the attach/detach discipline
// right.
//
// Direction of travel:
//   Kotlin -> native   lifecycle, surface, input, settings
//   native -> Kotlin   fatal errors, quit, clipboard, URLs, the soft keyboard
//
// The second direction uses a cached global reference to the activity plus
// pre-resolved method ids, because resolving them per call would mean a class
// lookup on every clipboard read.

#include <jni.h>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "touch_controls.h"
#include "../gfx/gpu_backend.h"
#include "../perf/perf_director.h"
#include "../platform/android_platform.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

// ports/android/engine/android_input.cpp. Declared here rather than given a
// header of their own: these four are the entire surface between the JNI
// layer and the gamepad state, and they have exactly one caller each.
void KisakAndroid_GamepadConnected(bool connected);
void KisakAndroid_GamepadButton(int bit, bool pressed);
void KisakAndroid_GamepadAxes(float leftX, float leftY, float rightX, float rightY, float leftTrigger,
                              float rightTrigger);
bool KisakAndroid_GamepadPresent();

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KisakCOD", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KisakCOD", __VA_ARGS__)

namespace {

JavaVM *g_vm = nullptr;
jobject g_activity = nullptr; // global reference, released in onDestroy

struct ActivityMethods
{
    jmethodID showFatalError = nullptr;
    jmethodID requestQuit = nullptr;
    jmethodID openUrl = nullptr;
    jmethodID getClipboard = nullptr;
    jmethodID setClipboard = nullptr;
    jmethodID showRestartPrompt = nullptr;
    jmethodID setSoftKeyboardVisible = nullptr;
    jmethodID setFrameRate = nullptr;
};

ActivityMethods g_methods;
std::mutex g_clipboardMutex;
std::string g_clipboardText;

ANativeWindow *g_window = nullptr;

// Attaches the calling thread for the duration of a scope. The engine thread
// is not a Java thread, so every native -> Kotlin call needs this.
class ScopedEnv
{
public:
    ScopedEnv()
    {
        if (!g_vm)
            return;
        const jint status = g_vm->GetEnv(reinterpret_cast<void **>(&m_env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED)
        {
            JavaVMAttachArgs args{ JNI_VERSION_1_6, "KisakCOD engine", nullptr };
            if (g_vm->AttachCurrentThread(&m_env, &args) == JNI_OK)
                m_attached = true;
            else
                m_env = nullptr;
        }
        else if (status != JNI_OK)
        {
            m_env = nullptr;
        }
    }

    ~ScopedEnv()
    {
        if (m_attached && g_vm)
            g_vm->DetachCurrentThread();
    }

    ScopedEnv(const ScopedEnv &) = delete;
    ScopedEnv &operator=(const ScopedEnv &) = delete;

    JNIEnv *operator->() const { return m_env; }
    explicit operator bool() const { return m_env != nullptr && g_activity != nullptr; }
    JNIEnv *get() const { return m_env; }

private:
    JNIEnv *m_env = nullptr;
    bool m_attached = false;
};

std::string ToUtf8(JNIEnv *env, jstring value)
{
    if (!value)
        return {};
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(value, chars);
    return result;
}

// A Java exception left pending turns the next JNI call into a crash with a
// confusing stack. Clearing it here keeps a failed callback from taking the
// engine down.
void ClearPendingException(JNIEnv *env)
{
    if (env && env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// native -> Kotlin. Declared extern by ports/android/engine/android_sys.cpp.

void KisakAndroid_ShowFatalError(const char *message)
{
    ScopedEnv env;
    if (!env || !g_methods.showFatalError)
    {
        // The VM is gone or the activity never attached; logcat is all that
        // is left, and Sys_Error is about to abort anyway.
        LOGE("fatal: %s", message ? message : "(null)");
        return;
    }
    jstring text = env->NewStringUTF(message ? message : "");
    env->CallVoidMethod(g_activity, g_methods.showFatalError, text);
    ClearPendingException(env.get());
    env->DeleteLocalRef(text);
}

void KisakAndroid_RequestQuit()
{
    ScopedEnv env;
    if (!env || !g_methods.requestQuit)
        return;
    env->CallVoidMethod(g_activity, g_methods.requestQuit);
    ClearPendingException(env.get());
}

void KisakAndroid_OpenURL(const char *url)
{
    ScopedEnv env;
    if (!env || !g_methods.openUrl || !url)
        return;
    jstring text = env->NewStringUTF(url);
    env->CallVoidMethod(g_activity, g_methods.openUrl, text);
    ClearPendingException(env.get());
    env->DeleteLocalRef(text);
}

const char *KisakAndroid_GetClipboardText()
{
    ScopedEnv env;
    if (!env || !g_methods.getClipboard)
        return nullptr;

    auto result = static_cast<jstring>(env->CallObjectMethod(g_activity, g_methods.getClipboard));
    ClearPendingException(env.get());
    if (!result)
        return nullptr;

    // The engine expects a pointer it does not own and does not free, so the
    // text is held here until the next read.
    std::lock_guard<std::mutex> lock(g_clipboardMutex);
    g_clipboardText = ToUtf8(env.get(), result);
    env->DeleteLocalRef(result);
    return g_clipboardText.empty() ? nullptr : g_clipboardText.c_str();
}

void KisakAndroid_SetClipboardText(const char *text)
{
    ScopedEnv env;
    if (!env || !g_methods.setClipboard)
        return;
    jstring value = env->NewStringUTF(text ? text : "");
    env->CallVoidMethod(g_activity, g_methods.setClipboard, value);
    ClearPendingException(env.get());
    env->DeleteLocalRef(value);
}

void KisakAndroid_ShowRestartPrompt(const char *mode)
{
    ScopedEnv env;
    if (!env || !g_methods.showRestartPrompt)
        return;
    jstring value = env->NewStringUTF(mode ? mode : "");
    env->CallVoidMethod(g_activity, g_methods.showRestartPrompt, value);
    ClearPendingException(env.get());
    env->DeleteLocalRef(value);
}

// The engine-visible KisakAndroid_SetSoftKeyboardVisible lives in
// android_sys.cpp with the rest of the input state and calls this.
void KisakAndroid_PlatformSoftKeyboard(int visible)
{
    ScopedEnv env;
    if (!env || !g_methods.setSoftKeyboardVisible)
        return;
    env->CallVoidMethod(g_activity, g_methods.setSoftKeyboardVisible, static_cast<jboolean>(visible != 0));
    ClearPendingException(env.get());
}

// The frame pacer asks the display for a specific refresh rate when the one
// it is targeting is not the current mode. Surface.setFrameRate is the only
// way to do that, and it is Java-only.
void KisakAndroid_RequestFrameRate(float hz)
{
    ScopedEnv env;
    if (!env || !g_methods.setFrameRate)
        return;
    env->CallVoidMethod(g_activity, g_methods.setFrameRate, static_cast<jfloat>(hz));
    ClearPendingException(env.get());
}

// ---------------------------------------------------------------------------
// Renderer hooks the system layer calls by name.

int64_t KisakAndroid_LastGpuFrameTimeNs()
{
    return kisak::vk::LastGpuFrameTimeNs();
}

void KisakAndroid_SetRenderResolution(uint32_t width, uint32_t height)
{
    kisak::vk::SetRenderResolution(width, height);
}

// ---------------------------------------------------------------------------
// Kotlin -> native

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *)
{
    g_vm = vm;
    return JNI_VERSION_1_6;
}

#define JNI_METHOD(returnType, name) \
    JNIEXPORT returnType JNICALL Java_ovh_kisak_cod4_EngineBridge_##name

JNI_METHOD(void, nativeAttach)(JNIEnv *env, jobject thiz, jobject activity)
{
    if (g_activity)
        env->DeleteGlobalRef(g_activity);
    g_activity = env->NewGlobalRef(activity);

    jclass klass = env->GetObjectClass(activity);
    g_methods.showFatalError = env->GetMethodID(klass, "showFatalError", "(Ljava/lang/String;)V");
    g_methods.requestQuit = env->GetMethodID(klass, "requestQuit", "()V");
    g_methods.openUrl = env->GetMethodID(klass, "openUrl", "(Ljava/lang/String;)V");
    g_methods.getClipboard = env->GetMethodID(klass, "getClipboardText", "()Ljava/lang/String;");
    g_methods.setClipboard = env->GetMethodID(klass, "setClipboardText", "(Ljava/lang/String;)V");
    g_methods.showRestartPrompt = env->GetMethodID(klass, "showRestartPrompt", "(Ljava/lang/String;)V");
    g_methods.setSoftKeyboardVisible = env->GetMethodID(klass, "setSoftKeyboardVisible", "(Z)V");
    g_methods.setFrameRate = env->GetMethodID(klass, "setPreferredFrameRate", "(F)V");
    ClearPendingException(env);
    env->DeleteLocalRef(klass);
    (void)thiz;
}

JNI_METHOD(void, nativeDetach)(JNIEnv *env, jobject)
{
    if (g_activity)
    {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
    g_methods = ActivityMethods{};
}

JNI_METHOD(void, nativeSetStorageRoots)
(JNIEnv *env, jobject, jstring gameData, jstring internalCache, jstring externalCache)
{
    const std::string data = ToUtf8(env, gameData);
    const std::string internal = ToUtf8(env, internalCache);
    const std::string external = ToUtf8(env, externalCache);
    KisakAndroid_SetStorageRoots(data.c_str(), internal.c_str(), external.c_str());
    // Has to happen before the surface arrives and the renderer initialises:
    // the shader and pipeline caches are read during device creation, and a
    // directory set afterwards would only take effect from the second launch.
    kisak::vk::SetCacheDirectory(internal.c_str());
    LOGI("game data: %s", data.c_str());
}

JNI_METHOD(void, nativeSurfaceCreated)(JNIEnv *env, jobject, jobject surface)
{
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (g_window)
        ANativeWindow_release(g_window);
    g_window = window;

    const int width = window ? ANativeWindow_getWidth(window) : 0;
    const int height = window ? ANativeWindow_getHeight(window) : 0;
    KisakAndroid_SetNativeWindow(window, width, height);
    kisak::android::touch::SetBounds(0, 0, static_cast<float>(width), static_cast<float>(height));
    LOGI("surface created %dx%d", width, height);
}

JNI_METHOD(void, nativeSurfaceChanged)(JNIEnv *env, jobject, jobject surface, jint width, jint height)
{
    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (g_window)
        ANativeWindow_release(g_window);
    g_window = window;

    KisakAndroid_SetNativeWindow(window, width, height);
    kisak::vk::SurfaceChanged(window, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    kisak::android::touch::SetBounds(0, 0, static_cast<float>(width), static_cast<float>(height));
    LOGI("surface changed %dx%d", width, height);
}

JNI_METHOD(void, nativeSurfaceDestroyed)(JNIEnv *, jobject)
{
    // The engine thread must not touch the window after this returns, so the
    // renderer is told first and the handle is cleared before the release.
    kisak::vk::SurfaceChanged(nullptr, 0, 0);
    KisakAndroid_SetNativeWindow(nullptr, 0, 0);
    if (g_window)
    {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }
    kisak::android::touch::CancelAll();
    LOGI("surface destroyed");
}

JNI_METHOD(jboolean, nativeStartEngine)(JNIEnv *env, jobject, jstring commandLine)
{
    const std::string line = ToUtf8(env, commandLine);
    return KisakAndroid_StartEngine(line.c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(void, nativeSetForeground)(JNIEnv *, jobject, jboolean foreground)
{
    KisakAndroid_SetForeground(foreground == JNI_TRUE);
    if (foreground != JNI_TRUE)
    {
        // Pausing is the last reliable moment to write the pipeline cache:
        // an app swiped out of recents never reaches onDestroy.
        kisak::vk::SavePipelineCache();
        kisak::android::touch::CancelAll();
    }
}

JNI_METHOD(void, nativeSetDisplayMetrics)
(JNIEnv *, jobject, jint width, jint height, jfloat refreshHz, jfloat safeHorizontal, jfloat safeVertical)
{
    KisakAndroid_SetDisplayRefreshRate(refreshHz);
    KisakAndroid_SetDisplaySafeArea(safeHorizontal, safeVertical);
    kisak::android::touch::SetBounds(safeHorizontal, safeVertical,
                                     static_cast<float>(width) - 2.0f * safeHorizontal,
                                     static_cast<float>(height) - 2.0f * safeVertical);
}

JNI_METHOD(void, nativeThermalStatus)(JNIEnv *, jobject, jint status)
{
    KisakAndroid_OnThermalStatus(status);
}

JNI_METHOD(void, nativeThermalHeadroom)(JNIEnv *, jobject, jfloat headroom)
{
    KisakAndroid_OnThermalHeadroom(headroom);
}

JNI_METHOD(void, nativePowerSaveMode)(JNIEnv *, jobject, jboolean enabled)
{
    KisakAndroid_OnPowerSaveMode(enabled == JNI_TRUE);
}

// Touch. phase: 0 down, 1 move, 2 up, 3 cancel - the same encoding
// KisakAndroid_TouchEvent uses, so a device without the overlay (a tablet
// with a mouse, say) still drives the engine cursor.
JNI_METHOD(void, nativeTouch)(JNIEnv *, jobject, jint pointerId, jint phase, jfloat x, jfloat y)
{
    switch (phase)
    {
    case 0: kisak::android::touch::Down(pointerId, x, y); break;
    case 1: kisak::android::touch::Move(pointerId, x, y); break;
    case 2: kisak::android::touch::Up(pointerId); break;
    default: kisak::android::touch::CancelPointer(pointerId); break;
    }
    KisakAndroid_TouchEvent(pointerId, phase, x, y);
}

// --- gamepad ---------------------------------------------------------------
// Buttons are passed as a bit index into kisak::controller::Button rather
// than an Android keycode, so the keycode table lives in Kotlin next to the
// KeyEvent constants and this side stays free of them.

JNI_METHOD(void, nativeGamepadConnected)(JNIEnv *, jobject, jboolean connected)
{
    KisakAndroid_GamepadConnected(connected == JNI_TRUE);
}

JNI_METHOD(void, nativeGamepadButton)(JNIEnv *, jobject, jint bit, jboolean pressed)
{
    KisakAndroid_GamepadButton(bit, pressed == JNI_TRUE);
}

JNI_METHOD(void, nativeGamepadAxes)
(JNIEnv *, jobject, jfloat leftX, jfloat leftY, jfloat rightX, jfloat rightY, jfloat leftTrigger,
 jfloat rightTrigger)
{
    KisakAndroid_GamepadAxes(leftX, leftY, rightX, rightY, leftTrigger, rightTrigger);
}

JNI_METHOD(jboolean, nativeGamepadPresent)(JNIEnv *, jobject)
{
    return KisakAndroid_GamepadPresent() ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(void, nativeSetTouchControlsEnabled)(JNIEnv *, jobject, jboolean enabled)
{
    kisak::android::touch::SetEnabled(enabled == JNI_TRUE);
}

JNI_METHOD(jboolean, nativeTouchControlsVisible)(JNIEnv *, jobject)
{
    return kisak::android::touch::Visible() ? JNI_TRUE : JNI_FALSE;
}

// Layout query for the overlay. Returns the number of entries written into
// `out` as [x, y, w, h, isStick] tuples, so the whole layout crosses JNI in
// one call per frame rather than five calls per button.
JNI_METHOD(jint, nativeTouchControlLayout)(JNIEnv *env, jobject, jfloatArray out)
{
    const jsize capacity = env->GetArrayLength(out);
    jfloat *values = env->GetFloatArrayElements(out, nullptr);
    if (!values)
        return 0;

    jint written = 0;
    const size_t count = kisak::android::touch::ControlCount();
    for (size_t i = 0; i < count && (written + 1) * 5 <= capacity; ++i)
    {
        float x = 0, y = 0, w = 0, h = 0;
        bool isStick = false;
        if (!kisak::android::touch::ControlFrame(i, &x, &y, &w, &h, nullptr, &isStick))
            continue;
        values[written * 5 + 0] = x;
        values[written * 5 + 1] = y;
        values[written * 5 + 2] = w;
        values[written * 5 + 3] = h;
        values[written * 5 + 4] = isStick ? 1.0f : 0.0f;
        ++written;
    }

    env->ReleaseFloatArrayElements(out, values, 0);
    return written;
}

JNI_METHOD(jobjectArray, nativeTouchControlLabels)(JNIEnv *env, jobject)
{
    const size_t count = kisak::android::touch::ControlCount();
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(count), stringClass, nullptr);
    for (size_t i = 0; i < count; ++i)
    {
        const char *label = nullptr;
        kisak::android::touch::ControlFrame(i, nullptr, nullptr, nullptr, nullptr, &label, nullptr);
        jstring value = env->NewStringUTF(label ? label : "");
        env->SetObjectArrayElement(result, static_cast<jsize>(i), value);
        env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(stringClass);
    return result;
}

// Text entry from the soft keyboard.
JNI_METHOD(void, nativeTextInput)(JNIEnv *env, jobject, jstring text)
{
    const std::string value = ToUtf8(env, text);
    KisakAndroid_TextInput(value.c_str());
}

JNI_METHOD(void, nativeTextBackspace)(JNIEnv *, jobject)
{
    KisakAndroid_TextBackspace();
}

JNI_METHOD(void, nativeTextReturn)(JNIEnv *, jobject)
{
    KisakAndroid_TextReturn();
}

JNI_METHOD(jboolean, nativeTextInputActive)(JNIEnv *, jobject)
{
    return KisakAndroid_TextInputActive() ? JNI_TRUE : JNI_FALSE;
}

// Engine mode (single player or multiplayer). Changing it needs a process
// restart, because the two are separate shared libraries: KISAK_MP changes
// structure layouts throughout the engine, so one binary cannot be both.
JNI_METHOD(void, nativeSetEngineMode)(JNIEnv *env, jobject, jstring mode)
{
    const std::string value = ToUtf8(env, mode);
    KisakAndroid_SetEngineMode(value.c_str());
}

JNI_METHOD(jstring, nativeGetEngineMode)(JNIEnv *env, jobject)
{
    const char *mode = KisakAndroid_GetEngineMode();
    return env->NewStringUTF(mode ? mode : "mp");
}

// Performance telemetry for the in-app overlay and the launcher's diagnostics
// screen. Packed into one array because this is polled a few times a second
// and a JNI call per field would be eight times the boundary crossings for
// the same six numbers.
JNI_METHOD(jfloatArray, nativePerfStats)(JNIEnv *env, jobject)
{
    const kisak::perf::PerfDirector &director = kisak::perf::Director();
    const kisak::perf::FramePacer::Stats pacer = director.pacer().stats();
    const kisak::perf::ThermalGovernor::State thermal = director.thermal().state();

    const jfloat values[8] = {
        static_cast<jfloat>(pacer.presentedFps),
        static_cast<jfloat>(pacer.averageCpuMs),
        static_cast<jfloat>(pacer.averageGpuMs),
        static_cast<jfloat>(pacer.p95FrameMs),
        static_cast<jfloat>(pacer.missRatio),
        static_cast<jfloat>(director.scaler().scale()),
        static_cast<jfloat>(thermal.targetFps),
        static_cast<jfloat>(static_cast<int>(thermal.status)),
    };
    jfloatArray result = env->NewFloatArray(8);
    env->SetFloatArrayRegion(result, 0, 8, values);
    return result;
}

JNI_METHOD(jstring, nativePerfStatusLine)(JNIEnv *env, jobject)
{
    return env->NewStringUTF(kisak::perf::Director().StatusLine().c_str());
}

// Launcher settings that change how the director behaves. Applied live, so a
// change in the pause menu takes effect on the next frame.
JNI_METHOD(void, nativeSetDynamicResolution)(JNIEnv *, jobject, jboolean enabled)
{
    kisak::perf::Director().SetDynamicResolutionEnabled(enabled == JNI_TRUE);
}

JNI_METHOD(void, nativeSetRenderScale)(JNIEnv *, jobject, jfloat scale)
{
    kisak::perf::Director().SetManualRenderScale(scale);
}

JNI_METHOD(void, nativeSetTargetFps)(JNIEnv *, jobject, jint fps)
{
    kisak::perf::Director().SetTargetFps(fps);
}

} // extern "C"
