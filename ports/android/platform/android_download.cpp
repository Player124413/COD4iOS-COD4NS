// HTTP downloads (mods, custom maps, the CoD4x compatibility patch) for the
// Android multiplayer build.
//
// There is no NDK HTTP or TLS API. OkHttp lives in Java, Cronet lives in
// Java, and the system CA store is only reachable through
// javax.net.ssl.TrustManager. Linking a private copy of BoringSSL into the
// engine would mean shipping and maintaining our own root store, which is a
// worse answer for a sideloaded app than crossing into Java.
//
// So this is a JNI bridge with the same C surface as the iOS version
// (ports/ios/platform/apple_download.h) so src/client_mp sees one API. The
// Java side (DownloadBridge.kt) runs the transfer on its own thread and
// writes straight to the destination file; the engine polls, exactly as it
// polls NSURLSession on iOS.
//
// Thread attachment is the fiddly part: the engine calls these from its own
// pthread, which the JVM knows nothing about, so every entry point attaches
// as a daemon thread and leaves itself attached. Detaching per call costs
// roughly 50 microseconds and the engine polls every frame.

#include "android_download.h"
#include "android_platform.h"

#include <jni.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

JavaVM *g_vm = nullptr;
jclass g_bridgeClass = nullptr;
jmethodID g_begin = nullptr;
jmethodID g_poll = nullptr;
jmethodID g_cancel = nullptr;
jmethodID g_received = nullptr;
jmethodID g_expected = nullptr;

std::mutex g_mutex;
std::atomic<bool> g_inProgress{false};
std::atomic<bool> g_isMotd{false};

// Attaches the calling thread if needed and returns its JNIEnv, or null when
// the bridge is not registered (single-player builds, or a Java layer that
// failed to initialise).
JNIEnv *Env()
{
    if (!g_vm || !g_bridgeClass)
        return nullptr;
    JNIEnv *env = nullptr;
    const jint status = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK)
        return env;
    if (status != JNI_EDETACHED)
        return nullptr;
    JavaVMAttachArgs args{};
    args.version = JNI_VERSION_1_6;
    args.name = const_cast<char *>("KisakDownload");
    args.group = nullptr;
    // Daemon attachment: the VM will not wait for this thread at shutdown,
    // which matters because the engine thread never returns.
    if (g_vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK)
        return nullptr;
    return env;
}

} // namespace

// Called once from JNI_OnLoad in ports/android/app/jni_bridge.cpp.
void KisakAndroid_RegisterDownloadBridge(JavaVM *vm, JNIEnv *env, jclass bridgeClass)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_vm = vm;
    // A local class reference dies with the frame that produced it; the
    // engine calls into this long afterwards from another thread.
    g_bridgeClass = static_cast<jclass>(env->NewGlobalRef(bridgeClass));
    g_begin = env->GetStaticMethodID(g_bridgeClass, "begin", "(Ljava/lang/String;Ljava/lang/String;)Z");
    g_poll = env->GetStaticMethodID(g_bridgeClass, "poll", "()I");
    g_cancel = env->GetStaticMethodID(g_bridgeClass, "cancel", "()V");
    g_received = env->GetStaticMethodID(g_bridgeClass, "bytesReceived", "()J");
    g_expected = env->GetStaticMethodID(g_bridgeClass, "bytesExpected", "()J");
}

extern "C" int KisakDownload_Begin(const char *localName, const char *remoteName, int isMotd)
{
    if (!localName || !remoteName)
        return 0;
    JNIEnv *env = Env();
    if (!env || !g_begin)
        return 0;

    // The engine passes a path relative to the game data folder; Java needs
    // an absolute one, and resolving it here keeps the storage layout in one
    // place (android_storage.cpp) rather than duplicated across the bridge.
    std::string destination = KisakAndroid_GameDataPath();
    if (!destination.empty() && localName[0] != '/')
        destination += '/';
    destination += localName;

    jstring jDestination = env->NewStringUTF(destination.c_str());
    jstring jUrl = env->NewStringUTF(remoteName);
    const jboolean started = env->CallStaticBooleanMethod(g_bridgeClass, g_begin, jDestination, jUrl);
    env->DeleteLocalRef(jDestination);
    env->DeleteLocalRef(jUrl);
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return 0;
    }

    g_inProgress.store(started == JNI_TRUE, std::memory_order_release);
    g_isMotd.store(isMotd != 0, std::memory_order_release);
    return started == JNI_TRUE ? 1 : 0;
}

extern "C" int KisakDownload_Poll(void)
{
    // dlStatus_t: 0 continue, 1 done, 2 failed.
    if (!g_inProgress.load(std::memory_order_acquire))
        return 2;
    JNIEnv *env = Env();
    if (!env || !g_poll)
        return 2;
    const jint status = env->CallStaticIntMethod(g_bridgeClass, g_poll);
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
        g_inProgress.store(false, std::memory_order_release);
        return 2;
    }
    if (status != 0)
        g_inProgress.store(false, std::memory_order_release);
    return static_cast<int>(status);
}

extern "C" void KisakDownload_Cancel(void)
{
    JNIEnv *env = Env();
    if (env && g_cancel)
    {
        env->CallStaticVoidMethod(g_bridgeClass, g_cancel);
        if (env->ExceptionCheck())
            env->ExceptionClear();
    }
    g_inProgress.store(false, std::memory_order_release);
}

extern "C" int KisakDownload_InProgress(void)
{
    return g_inProgress.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int KisakDownload_IsMotd(void)
{
    return g_isMotd.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void KisakDownload_Progress(long long *received, long long *expected)
{
    if (received)
        *received = 0;
    if (expected)
        *expected = 0;
    JNIEnv *env = Env();
    if (!env || !g_received || !g_expected)
        return;
    if (received)
        *received = static_cast<long long>(env->CallStaticLongMethod(g_bridgeClass, g_received));
    if (expected)
        *expected = static_cast<long long>(env->CallStaticLongMethod(g_bridgeClass, g_expected));
    if (env->ExceptionCheck())
        env->ExceptionClear();
}
