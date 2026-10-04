// Sys_* services and the startup sequence for Android, replacing the Win32
// bodies in src/win32 (win_main.cpp, win_sys.cpp, win_input.cpp).
//
// Mirrors ports/ios/engine/apple_sys.cpp in structure; the differences are
// where Android genuinely differs:
//
//   * Hardware discovery reads /proc and the properties the Java layer passes
//     down, not sysctl.
//   * The frame loop is paced (ports/android/perf) instead of free-running.
//     The iOS port leaves com_maxfps at 0 and lets CADisplayLink throttle it;
//     Android has no equivalent guarantee, and an unpaced loop on a phone
//     heats the SoC until the clocks collapse.
//   * Backgrounding is explicit. Android will kill an app that keeps a core
//     busy behind the lock screen, so the loop drops to a slow tick when the
//     activity is not resumed.
//   * Console output goes to logcat as well as the log file; stderr alone is
//     not captured on a release build.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <qcommon/threads.h>
#include <universal/com_memory.h>
#include <win32/win_local.h>
#include <win32/win_localize.h>
#include <win32/win_input.h>
#include <universal/profile.h>
#include <universal/q_parse.h>
#include <universal/timing.h>

#include <client/client.h> // branches internally on SP/MP
#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#else
#include <client/cl_input.h>
#endif
#include <ui/keycodes.h>

#include "../perf/perf_director.h"
#include "../platform/android_log.h"
#include "../platform/android_platform.h"
#include "controller_input.h"

// --- implemented in ports/android/engine/android_input.cpp ---
void KisakAndroid_ControllerFrame();

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>
#include <string>

#include <pthread.h>
#include <thread>
#include <limits.h>
#include <unistd.h>
#include <sys/system_properties.h>

#define KISAK_LOG_TAG "KisakCOD"

namespace {

std::atomic<bool> g_foreground{true};
std::atomic<bool> g_engineRunning{false};
std::atomic<void *> g_nativeWindow{nullptr};
std::atomic<int> g_windowWidth{0};
std::atomic<int> g_windowHeight{0};
std::atomic<int> g_softKeyboardVisible{0};
float g_safeAreaHorizontal = 0.0f;
float g_safeAreaVertical = 0.0f;


std::string ReadSystemProperty(const char *name, const char *fallback)
{
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get(name, value);
    if (length <= 0)
        return std::string(fallback ? fallback : "");
    return std::string(value, static_cast<std::size_t>(length));
}

int ReadMemTotalMB()
{
    std::FILE *file = std::fopen("/proc/meminfo", "r");
    if (!file)
        return 1024;
    char line[256];
    long long kilobytes = 0;
    while (std::fgets(line, sizeof(line), file))
    {
        if (std::sscanf(line, "MemTotal: %lld kB", &kilobytes) == 1)
            break;
    }
    std::fclose(file);
    if (kilobytes <= 0)
        return 1024;
    const long long megabytes = kilobytes / 1024;
    // The engine's own code treats sysMB as a 32-bit value and the Windows
    // build caps it at 1 GB; keeping that cap avoids disturbing the hunk
    // sizing heuristics that were tuned against it.
    return megabytes > 1024 ? 1024 : static_cast<int>(megabytes);
}

void ReadCpuName(char *buffer, std::size_t size)
{
    // Android hides the SoC marketing name from /proc/cpuinfo on arm64; the
    // part number there is the core ("AArch64 Processor rev 1"). ro.soc.model
    // is the authoritative name on Android 12+, with the board platform as a
    // fallback on older releases.
    std::string name = ReadSystemProperty("ro.soc.model", "");
    if (name.empty())
        name = ReadSystemProperty("ro.board.platform", "");
    if (name.empty())
        name = ReadSystemProperty("ro.hardware", "arm64");
    const std::string vendor = ReadSystemProperty("ro.soc.manufacturer", "");
    if (!vendor.empty())
        std::snprintf(buffer, size, "%s %s", vendor.c_str(), name.c_str());
    else
        std::snprintf(buffer, size, "%s", name.c_str());
}


} // namespace

// ---------------------------------------------------------------------------
// Events queued from the UI thread into the engine's event ring.

sysEvent_t eventQue[MAX_QUED_EVENTS];
int eventHead;
int eventTail;

// Engine globals that live in src/win32/win_main.cpp on Windows. That file is
// not in the Android source list (WIN32_SRC is excluded wholesale), so the
// port owns them, exactly as ports/ios/engine/apple_sys.cpp does.
char sys_cmdline[1024];
SysInfo sys_info;
int client_state;
HWND g_splashWnd;
WinVars_t g_wv;

cmd_function_s Sys_In_Restart_f_VAR;

namespace {
std::mutex g_eventMutex;
}

void __cdecl Sys_QueEvent(uint32_t time, sysEventType_t type, int value, int value2, int ptrLength, void *ptr)
{
    std::lock_guard<std::mutex> lock(g_eventMutex);
    sysEvent_t *event = &eventQue[eventHead & 255];
    if (eventHead - eventTail >= 256)
    {
        // The queue only overflows when the engine has stalled; dropping the
        // oldest entry keeps input responsive once it recovers, and freeing
        // the payload avoids leaking a buffer per dropped event.
        if (event->evPtr)
            Z_Free((char *)event->evPtr, 10);
        ++eventTail;
    }
    ++eventHead;
    event->evTime = time ? time : Sys_Milliseconds();
    event->evType = type;
    event->evValue = value;
    event->evValue2 = value2;
    event->evPtrLength = ptrLength;
    event->evPtr = ptr;
}

sysEvent_t *__cdecl Sys_GetEvent(sysEvent_t *result)
{
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        if (eventHead > eventTail)
        {
            *result = eventQue[eventTail & 255];
            ++eventTail;
            return result;
        }
    }
    memset(result, 0, sizeof(*result));
    result->evTime = Sys_Milliseconds();
    return result;
}

void Sys_ShutdownEvents()
{
    std::lock_guard<std::mutex> lock(g_eventMutex);
    while (eventHead > eventTail)
    {
        sysEvent_t &event = eventQue[eventTail & 255];
        if (event.evPtr)
            Z_Free(event.evPtr, 0);
        ++eventTail;
    }
    eventHead = eventTail = 0;
}

// ---------------------------------------------------------------------------
// Console output

void __cdecl Sys_Print(const char *msg)
{
    if (!msg || !*msg)
        return;
    // Logcat truncates at about 4 KB per message and strips nothing, so the
    // engine's colour codes stay visible; that is useful when reading a bug
    // report where the player pasted logcat rather than the log file.
    __android_log_write(ANDROID_LOG_INFO, KISAK_LOG_TAG, msg);
    KisakAndroid_LogWrite(msg);
}

void Sys_Error(const char *error, ...)
{
    char message[4096];
    va_list arguments;
    va_start(arguments, error);
    vsnprintf(message, sizeof(message), error, arguments);
    va_end(arguments);

    __android_log_write(ANDROID_LOG_FATAL, KISAK_LOG_TAG, message);
    KisakAndroid_LogPrintf("\nFATAL: %s\n", message);
    KisakAndroid_LogFlush();
    // Hand the text to the Java layer so the player sees a dialog instead of
    // the app vanishing. Implemented in ports/android/app/jni_bridge.cpp.
    KisakAndroid_ShowFatalError(message);

    // abort() rather than exit(): it produces a tombstone with a native
    // backtrace, which is the only usable artefact from a field crash.
    std::abort();
}

void __cdecl Sys_OutOfMemErrorInternal(const char *filename, int line)
{
    Sys_Error("Out of memory (%s:%d). Free storage and memory, then try again.", filename ? filename : "?", line);
}

void __cdecl Sys_NormalExit()
{
    KisakAndroid_LogWrite("\nEngine shut down normally.\n");
    KisakAndroid_LogFlush();
}

void __cdecl Sys_Quit()
{
    Sys_NormalExit();
    // finishAndRemoveTask() on the Java side; calling exit() directly leaves
    // the task card behind and the next launch restores a dead activity.
    KisakAndroid_RequestQuit();
    // The engine expects Sys_Quit never to return.
    for (;;)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void __cdecl Sys_OpenURL(const char *url, int doexit)
{
    if (url && *url)
        KisakAndroid_OpenURL(url);
    if (doexit)
        Sys_Quit();
}

// ---------------------------------------------------------------------------
// Clipboard: routed through the Java ClipboardManager.

char *__cdecl Sys_GetClipboardData()
{
    const char *text = KisakAndroid_GetClipboardText();
    if (!text || !*text)
        return nullptr;
    const std::size_t length = std::strlen(text) + 1;
    // The engine frees this with Z_Free, so it has to come from the hunk.
    char *copy = static_cast<char *>(Z_Malloc(static_cast<int>(length), "clipboard", 0));
    if (copy)
        std::memcpy(copy, text, length);
    return copy;
}

int __cdecl Sys_SetClipboardData(const char *text)
{
    KisakAndroid_SetClipboardText(text ? text : "");
    return 1;
}

void __cdecl Sys_LoadingKeepAlive()
{
    // Android's ANR watchdog only looks at the main (UI) thread, and the
    // engine never runs there, so loading cannot trip it. Nothing to do,
    // but the engine calls it from deep inside fastfile loading and the
    // symbol must exist.
}

// ---------------------------------------------------------------------------
// Hardware discovery

void Sys_In_Restart_f()
{
    IN_Shutdown();
    IN_Init();
}

// Fills sys_info. Separate from Sys_Init because Com_Init runs autoconfigure
// - which picks the CPU/GPU tier and the texture detail from these fields -
// before it reaches Sys_Init. Left until then, autoconfigure reads zeroes and
// settles on "0 GHz 128 MB", which forces picmip 2 and blurs every texture.
// Idempotent, so Sys_Init can still call it on a path that skipped the early
// one.
void KisakAndroid_FillSystemInfo()
{
    static bool filled = false;
    if (filled)
        return;
    filled = true;

    sys_info.logicalCpuCount = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    if (sys_info.logicalCpuCount <= 0)
        sys_info.logicalCpuCount = 4;
    // Phones do not use SMT, so physical and logical counts are the same.
    sys_info.physicalCpuCount = sys_info.logicalCpuCount;

    // The Windows build benchmarks x86 cores to choose default detail. That
    // number is meaningless here, and the port chooses detail from the device
    // profile instead; report a plausible figure so the engine's own
    // thresholds behave.
    sys_info.cpuGHz = 2.4;
    sys_info.configureGHz = sys_info.cpuGHz * (sys_info.physicalCpuCount > 2 ? 2 : sys_info.physicalCpuCount);
    sys_info.sysMB = ReadMemTotalMB();
    sys_info.SSE = 0; // no x86 SIMD paths on arm64

    I_strncpyz(sys_info.cpuVendor, ReadSystemProperty("ro.soc.manufacturer", "ARM").c_str(),
               sizeof(sys_info.cpuVendor));
    ReadCpuName(sys_info.cpuName, sizeof(sys_info.cpuName));
    I_strncpyz(sys_info.gpuDescription, kisak::perf::Director().profile().tier == kisak::perf::QualityTier::Ultra
                                            ? "Android GPU (Vulkan)"
                                            : "Android GPU",
               sizeof(sys_info.gpuDescription));

}

void __cdecl Sys_Init()
{
    Cmd_AddCommandInternal("in_restart", Sys_In_Restart_f, &Sys_In_Restart_f_VAR);

    KisakAndroid_FillSystemInfo();

    Com_Printf(CON_CHANNEL_SYSTEM, "CPU vendor is \"%s\"\n", sys_info.cpuVendor);
    Com_Printf(CON_CHANNEL_SYSTEM, "CPU name is \"%s\"\n", sys_info.cpuName);
    Com_Printf(CON_CHANNEL_SYSTEM, "%i logical CPUs reported\n", sys_info.logicalCpuCount);
    const kisak::perf::CpuTopology &topology = kisak::perf::Director().topology();
    Com_Printf(CON_CHANNEL_SYSTEM, "CPU clusters: %zu little, %zu big, %zu prime\n", topology.little.size(),
               topology.big.size(), topology.prime.size());
    Com_Printf(CON_CHANNEL_SYSTEM, "System memory is %i MB (capped at 1 GB)\n", sys_info.sysMB);
    Com_Printf(CON_CHANNEL_SYSTEM, "Video card is \"%s\"\n", sys_info.gpuDescription);
    Com_Printf(CON_CHANNEL_SYSTEM, "Quality tier: %s\n",
               kisak::perf::QualityTierName(kisak::perf::Director().profile().tier));
    Com_Printf(CON_CHANNEL_SYSTEM, "\n");
    IN_Init();
}

#ifndef KISAK_MP
void __cdecl NET_Init()
{
    // Single player never sends a packet; the multiplayer build gets the real
    // implementation from ports/android/platform/android_net.cpp.
}

void NET_Sleep(int msec)
{
    if (msec > 0)
        Sys_Sleep(static_cast<unsigned int>(msec));
}
#else
void __cdecl NET_Init()
{
    const dvar_t *clientPort = Dvar_RegisterInt("net_port", 28960, 0, 0xFFFF, DVAR_LATCH, "Network port");
    const dvar_t *serverPort = Dvar_RegisterInt("net_serverPort", 28960, 0, 0xFFFF, DVAR_LATCH, "Server network port");
    // The client binds an ephemeral port so it can run alongside a server.
    Sys_InitNetworking(static_cast<uint16_t>(clientPort->current.integer + 1),
                       static_cast<uint16_t>(serverPort->current.integer));
}
#endif

// ---------------------------------------------------------------------------
// Remote script debugger: a Windows development tool, never connected here.

int g_debugClient;
unsigned __int8 g_debugPacket[1][8192];

int __cdecl Sys_IsRemoteDebugClient() { return 0; }
void __cdecl NET_ShutdownDebug() {}
void NET_InitDebug() {}
void NET_RestartDebug() {}
void __cdecl Sys_Listen_f() {}
void Sys_DebugSocketError(const char *message) { (void)message; }
int __cdecl Sys_ReadDebugSocketInt() { return 0; }
void __cdecl Sys_WriteDebugSocketInt(int value) { (void)value; }
void __cdecl Sys_WriteDebugSocketString(char *text) { (void)text; }
int __cdecl Sys_ReadDebugSocketMessageType(unsigned __int8 *type, int blocking)
{
    (void)type;
    (void)blocking;
    return 0;
}
int __cdecl Sys_UpdateDebugSocket() { return 0; }
int __cdecl Sys_ReadDebugSocketData(char *buffer, int len, int blocking)
{
    (void)buffer;
    (void)len;
    (void)blocking;
    return 0;
}
void __cdecl Sys_ReadDebugSocketStringBuffer(char *buffer, int len)
{
    if (len > 0)
        buffer[0] = 0;
}
void __cdecl Sys_FlushDebugSocketData() {}
void __cdecl Sys_AckDebugSocket() {}
char *__cdecl Sys_ReadDebugSocketString()
{
    static char empty[1];
    return empty;
}
void __cdecl Sys_WriteDebugSocketData(unsigned __int8 *buffer, int len)
{
    (void)buffer;
    (void)len;
}
void __cdecl Sys_WriteDebugSocketMessageType(unsigned __int8 type) { (void)type; }
void __cdecl Sys_EndWriteDebugSocket() {}

// ---------------------------------------------------------------------------
// Input plumbing. The real work is in controller_input.cpp (shared with iOS)
// and ports/android/app/touch_controls.cpp.

void IN_Init() {}
void IN_Shutdown() {}

namespace {

struct KisakTouch
{
    int pointerId;
    int phase; // 0 down, 1 move, 2 up, 3 cancel
    int x;
    int y;
};

// Touches arrive on the UI thread and are consumed by IN_Frame on the engine
// thread. Queued rather than injected directly because CL_MouseEvent is not
// safe to call from another thread, and because the engine wants them in
// frame order.
std::mutex g_touchLock;
std::vector<KisakTouch> g_touches;

int g_cursorX = 0;
int g_cursorY = 0;
bool g_haveCursor = false;

// Android reports a move per pointer per sample; a swipe can produce dozens
// in one frame. The queue is bounded so a frame that takes 200 ms (a level
// load) cannot accumulate megabytes of stale input.
constexpr std::size_t kMaxQueuedTouches = 512;

} // namespace

void KisakAndroid_TouchEvent(int pointerId, int phase, float x, float y)
{
    std::lock_guard<std::mutex> guard(g_touchLock);
    if (g_touches.size() >= kMaxQueuedTouches)
    {
        // Drop the oldest move rather than the newest: the newest is where
        // the finger actually is, and losing it makes the cursor lag.
        g_touches.erase(g_touches.begin());
    }
    g_touches.push_back(KisakTouch{ pointerId, phase, static_cast<int>(x), static_cast<int>(y) });
}

// Controller-driven menu cursor: moves the cursor that touches use, in render
// target pixels, and optionally presses or releases the left mouse button
// (button: 1 down, 0 up, -1 none). Called from IN_Frame by the shared
// controller code through KisakApple_ControllerCursor.
void KisakAndroid_ControllerCursor(float dx, float dy, int button)
{
    int width = 0;
    int height = 0;
    KisakAndroid_GetDisplaySize(&width, &height);

    if (!g_haveCursor)
    {
        g_cursorX = width / 2;
        g_cursorY = height / 2;
        g_haveCursor = true;
    }

    const int maxX = width > 0 ? width - 1 : 0;
    const int maxY = height > 0 ? height - 1 : 0;
    const int x = std::clamp(g_cursorX + static_cast<int>(std::lround(dx)), 0, maxX);
    const int y = std::clamp(g_cursorY + static_cast<int>(std::lround(dy)), 0, maxY);
    const int moveX = x - g_cursorX;
    const int moveY = y - g_cursorY;
    g_cursorX = x;
    g_cursorY = y;

    if (moveX || moveY || button >= 0)
        CL_MouseEvent(x, y, moveX, moveY);
    if (button >= 0)
        Sys_QueEvent(0, SE_KEY, K_MOUSE1, button, 0, nullptr);
}

void IN_Frame()
{
    KisakAndroid_ControllerFrame();

    // Follow the engine into and out of a text field. iOS does this from a
    // timer in -syncKeyboard; the Android plumbing existed down to the
    // Kotlin InputMethodManager call but nothing ever decided to show the
    // keyboard, so a phone with no hardware keys could not type a profile
    // name at all.
    {
        const int editing = KisakAndroid_TextInputActive() ? 1 : 0;
        if (editing != g_softKeyboardVisible.load(std::memory_order_acquire))
        {
            KisakAndroid_LogPrintf("text field %s editing; %s keyboard\n",
                                   editing ? "began" : "ended", editing ? "showing" : "hiding");
            KisakAndroid_SetSoftKeyboardVisible(editing);
        }
    }

    std::vector<KisakTouch> touches;
    {
        std::lock_guard<std::mutex> guard(g_touchLock);
        touches.swap(g_touches);
    }

    for (const KisakTouch &touch : touches)
    {
        if (touch.phase == 3)
        {
            // A cancelled gesture releases the button without moving the
            // cursor, so a system back swipe cannot leave fire held down.
            Sys_QueEvent(0, SE_KEY, K_MOUSE1, 0, 0, nullptr);
            continue;
        }

        const int dx = g_haveCursor ? touch.x - g_cursorX : 0;
        const int dy = g_haveCursor ? touch.y - g_cursorY : 0;
        g_cursorX = touch.x;
        g_cursorY = touch.y;
        g_haveCursor = true;

        // Move first, so a tap clicks where the finger landed rather than
        // where the cursor happened to be.
        CL_MouseEvent(touch.x, touch.y, dx, dy);
        if (touch.phase == 0)
            Sys_QueEvent(0, SE_KEY, K_MOUSE1, 1, 0, nullptr);
        else if (touch.phase == 2)
            Sys_QueEvent(0, SE_KEY, K_MOUSE1, 0, 0, nullptr);
    }
}

void __cdecl IN_ShowSystemCursor(BOOL show) { (void)show; }
void __cdecl IN_SetForegroundWindow() {}
bool __cdecl IN_IsForegroundWindow() { return g_foreground.load(std::memory_order_acquire); }
void IN_ActivateMouse(qboolean force) { (void)force; }

// src/ui/ui_shared.cpp. Set by Item_TextField_BeginEdit when a field takes
// focus and cleared when editing ends; this is the engine's own notion of
// "text is being typed", and the same global the iOS port reads.
extern int g_editingField;

int KisakAndroid_TextInputActive()
{
    // Must be the engine's state, not g_softKeyboardVisible: that one only
    // records what we last asked the IME for, so returning it here made the
    // keyboard poll read back its own output and never fire.
    return g_editingField;
}

void KisakAndroid_SetSoftKeyboardVisible(int visible)
{
    g_softKeyboardVisible.store(visible, std::memory_order_release);
    // Implemented in ports/android/app/jni_bridge.cpp, which owns the only
    // JNI calls in the port. Split in two so the engine-visible flag lives
    // next to the rest of the input state and the Java call stays on the
    // other side of the boundary.
    KisakAndroid_PlatformSoftKeyboard(visible);
}

void KisakAndroid_TextInput(const char *utf8)
{
    if (!utf8)
        return;
    // The engine's font and localisation path is Windows-1252; the Java layer
    // has already transcoded, so each byte is one character here.
    for (const char *c = utf8; *c; ++c)
        Sys_QueEvent(0, SE_CHAR, static_cast<unsigned char>(*c), 0, 0, nullptr);
}

void KisakAndroid_TextBackspace()
{
    Sys_QueEvent(0, SE_CHAR, 8, 0, 0, nullptr);
}

void KisakAndroid_TextReturn()
{
    Sys_QueEvent(0, SE_CHAR, 13, 0, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Display and window state

void KisakAndroid_SetNativeWindow(void *window, int width, int height)
{
    g_nativeWindow.store(window, std::memory_order_release);
    g_windowWidth.store(width, std::memory_order_release);
    g_windowHeight.store(height, std::memory_order_release);
    if (width > 0 && height > 0)
        kisak::perf::Director().SetNativeResolution(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void *KisakAndroid_GetNativeWindow()
{
    return g_nativeWindow.load(std::memory_order_acquire);
}

bool KisakAndroid_GetDisplaySize(int *width, int *height)
{
    const int w = g_windowWidth.load(std::memory_order_acquire);
    const int h = g_windowHeight.load(std::memory_order_acquire);
    if (w <= 0 || h <= 0)
        return false;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return true;
}

std::atomic<double> g_displayRefreshHz{ 60.0 };

double KisakAndroid_GetDisplayRefreshRate()
{
    return g_displayRefreshHz.load(std::memory_order_acquire);
}

void KisakAndroid_SetDisplayRefreshRate(double hz)
{
    if (hz > 20.0 && hz < 500.0)
        g_displayRefreshHz.store(hz, std::memory_order_release);
    kisak::perf::Director().SetDisplayRefreshRate(hz);
}

void KisakAndroid_SetDisplaySafeArea(float horizontal, float vertical)
{
    g_safeAreaHorizontal = horizontal;
    g_safeAreaVertical = vertical;
}

void KisakAndroid_GetDisplaySafeArea(float *horizontal, float *vertical)
{
    if (horizontal)
        *horizontal = g_safeAreaHorizontal;
    if (vertical)
        *vertical = g_safeAreaVertical;
}

void KisakAndroid_SetForeground(bool foreground)
{
    g_foreground.store(foreground, std::memory_order_release);
}

bool KisakAndroid_IsForeground()
{
    return g_foreground.load(std::memory_order_acquire);
}

void KisakAndroid_OnThermalStatus(int status)
{
    kisak::perf::Director().OnThermalStatus(static_cast<kisak::perf::ThermalStatus>(
        status < 0 ? 0 : (status > 6 ? 6 : status)));
}

void KisakAndroid_OnThermalHeadroom(float headroom)
{
    kisak::perf::Director().OnThermalHeadroom(headroom);
}

void KisakAndroid_OnPowerSaveMode(bool enabled)
{
    // Fighting the battery saver for clocks it will not grant produces a
    // sawtooth frame rate. Dropping the target to 30 gives a steady one.
    kisak::perf::Director().SetTargetFps(enabled ? 30 : 60);
}

// ---------------------------------------------------------------------------
// Startup and the frame loop

int KisakAndroid_RunEngine(const char *commandLine)
{
    if (g_engineRunning.exchange(true))
        return 0;

    // Both are idempotent: the launcher already did this through
    // nativeSetLogPath before the surface existed. Repeating it here covers
    // a host or test run that starts the engine without the Java layer.
    KisakAndroid_LogOpen();
    KisakAndroid_InstallCrashHandler();

    // Before anything that touches a file, and before anything that could
    // call Sys_DefaultInstallPath().
    //
    // The engine finds game data two ways, and on Android both start out
    // wrong. Win_InitLocalization fopen()s the bare name "localization.txt",
    // which resolves against the working directory - and an Android process
    // starts in "/". Sys_DefaultInstallPath() prefixes the zone paths
    // DB_BuildOSPath builds, and off Windows it takes $KISAK_INSTALL_PATH or
    // falls back to the working directory, caching whichever it got on the
    // first call. Left alone, that yields "/\zone\(null)\code_post_gfx.ff":
    // no install path, and no language because localization.txt was never
    // found.
    //
    // The iOS app chdir()s into the game folder and relies on the fallback.
    // Do that too, and set the variable as well, so a later caller cannot
    // depend on the working directory having survived.
    const char *gameData = KisakAndroid_GameDataPath();
    // Resolve it first. The launcher reports /data/user/0/<pkg>/..., which is
    // a symlink to /data/data/<pkg>/...; getcwd() below reports the latter,
    // and the engine would treat the two spellings as separate search paths
    // and index every iwd twice.
    char resolved[PATH_MAX];
    if (gameData && *gameData && realpath(gameData, resolved))
        gameData = resolved;
    if (!gameData || !*gameData || chdir(gameData) != 0)
    {
        char message[512];
        std::snprintf(message, sizeof(message),
            "Could not enter the game data folder:\n%s\n\n%s",
            (gameData && *gameData) ? gameData : "(no path was set)",
            (gameData && *gameData) ? std::strerror(errno) : "The launcher did not report one.");
        KisakAndroid_LogPrintf("FATAL: %s\n", message);
        KisakAndroid_LogFlush();
        KisakAndroid_ShowFatalError(message);
        g_engineRunning = false;
        return 1;
    }
    setenv("KISAK_INSTALL_PATH", gameData, 1);
    KisakAndroid_LogPrintf("install path: %s\ncommand line: %s\n\n",
        gameData, commandLine ? commandLine : "");

    // Win_InitLocalization does not fail loudly: when it cannot open this it
    // asserts, which is compiled out of a release build, and carries on with
    // a null language. The first symptom is the zone loader reporting
    // "Could not find zone '\zone\(null)\code_post_gfx.ff'", which names
    // neither the real problem nor the file. Check it here instead.
    if (std::FILE *const localization = std::fopen("localization.txt", "rb"))
    {
        std::fclose(localization);
    }
    else
    {
        char message[512];
        std::snprintf(message, sizeof(message),
            "localization.txt is missing from the game data:\n%s\n\n"
            "It names the language, and every zone path is built from it. "
            "Import the game folder again from the launcher.",
            gameData);
        KisakAndroid_LogPrintf("FATAL: %s\n", message);
        KisakAndroid_LogFlush();
        KisakAndroid_ShowFatalError(message);
        g_engineRunning = false;
        return 1;
    }

    kisak::perf::Director().PlaceThread(kisak::perf::ThreadRole::Render);

    Sys_InitializeCriticalSections();
    Sys_InitMainThread();
    track_init();
    Win_InitLocalization();

    Com_InitParse();
    Dvar_Init();
    InitTiming();
    // Before Com_Init, because it reports the quality tier and reads the CPU
    // topology. The GPU is still unknown at this point; the profile is
    // refined below once the renderer is up.
    KisakAndroid_BootstrapDeviceProfile();

    // Also before Com_Init: autoconfigure runs inside it and reads sys_info
    // long before Com_Init reaches Sys_Init.
    KisakAndroid_FillSystemInfo();
    I_strncpyz(sys_cmdline, commandLine ? commandLine : "", sizeof(sys_cmdline));
    Sys_Milliseconds();
    Profile_Init();
    Profile_InitContext(0);
    Com_Init(sys_cmdline);

    // Com_Init brought the renderer up, so the GPU model is known now and the
    // provisional profile can be replaced with a real one.
    const kisak::perf::QualityProfile &profile = KisakAndroid_RefreshDeviceProfile();

    // Applied after Com_Init, so it overrides whatever the archived config
    // holds: a profile copied from another device, or one written before the
    // player changed phones, must not decide the settings.
    const std::string defaults = profile.ToConsoleCommands();
    Cbuf_AddText(0, defaults.c_str());
    // The pacer owns the frame rate, so the engine's own limiter must not
    // also try. Two limiters in series produce a beat frequency, which is
    // exactly the uneven 50-ish fps that unpaced mobile ports are known for.
    Dvar_SetInt(com_maxfps, 0);

#ifdef KISAK_MP
    Cbuf_AddText(0, "readStats\n");
#endif
    Com_Printf(CON_CHANNEL_SYSTEM, "Game data: %s\n", KisakAndroid_GameDataPath());

    kisak::perf::PerfDirector &director = kisak::perf::Director();
    auto nowNs = [] {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
    };

    int64_t vsyncNs = nowNs();
    for (;;)
    {
        if (!g_foreground.load(std::memory_order_acquire))
        {
            // Backgrounded: keep simulating slowly so a multiplayer
            // connection survives a notification shade, but stop rendering
            // and stop burning a core. Android kills apps that do not.
            Com_Frame();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            vsyncNs = nowNs();
            continue;
        }

        director.BeginFrame(vsyncNs);
        Com_Frame();
        const int64_t cpuDone = nowNs();
        director.EndCpuWork(cpuDone);

        // The GPU backend reports the previous frame's timestamp range here;
        // a query cannot be read back in the frame that wrote it without a
        // stall, which would cost more than the measurement is worth.
        director.ReportGpuTime(KisakAndroid_LastGpuFrameTimeNs());

        const int64_t frameEnd = nowNs();
        const auto result = director.EndFrame(frameEnd);
        if (result.resolutionChanged)
        {
            KisakAndroid_SetRenderResolution(result.renderWidth, result.renderHeight);
        }

        // Hold back until the frame should start, so the SoC can drop clocks
        // instead of racing to a deadline it has already met.
        int64_t remaining = director.SleepBudgetNs(frameEnd);
        while (remaining > 0)
        {
            if (director.ShouldSpin(remaining))
            {
                // A sub-millisecond sleep routinely overshoots by more than
                // its own length on a loaded Android kernel; spinning the
                // last stretch is what keeps the cadence exact.
                asm volatile("yield" ::: "memory");
            }
            else
            {
                timespec request{ 0, static_cast<long>(remaining - 500000) };
                nanosleep(&request, nullptr);
            }
            remaining = director.SleepBudgetNs(nowNs());
        }
        vsyncNs = nowNs();
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Thread entry.
//
// The engine cannot run on the UI thread. A level load blocks for several
// seconds, and Android's watchdog turns five seconds of unresponsive UI into
// an ANR dialog and a kill. It also cannot be a Java thread: it is created
// before any JNI call is made and never attaches to the VM, which is what
// lets the frame loop run without taking the VM's thread-state locks sixty
// times a second.

namespace {

std::atomic<bool> g_engineStarted{ false };
pthread_t g_engineThread{};
std::string g_engineCommandLine;

void *EngineThreadMain(void *)
{
    // Named so it is identifiable in a tombstone and in Systrace, where it is
    // otherwise one of a dozen anonymous threads.
    pthread_setname_np(pthread_self(), "KisakCOD engine");
    kisak::perf::Director().PlaceThread(kisak::perf::ThreadRole::Render);
    KisakAndroid_RunEngine(g_engineCommandLine.c_str());
    return nullptr;
}

} // namespace

bool KisakAndroid_StartEngine(const char *commandLine)
{
    bool expected = false;
    if (!g_engineStarted.compare_exchange_strong(expected, true))
        return false;

    g_engineCommandLine = commandLine ? commandLine : "";

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // 16 MB, matching the iOS port. The engine recurses deeply through the
    // BSP and the script VM, and the 1 MB bionic default overflows during
    // the first level load - as a SIGSEGV with no useful stack, because the
    // guard page is hit inside the recursion.
    pthread_attr_setstacksize(&attributes, 16 * 1024 * 1024);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

    const int error = pthread_create(&g_engineThread, &attributes, EngineThreadMain, nullptr);
    pthread_attr_destroy(&attributes);

    if (error != 0)
    {
        __android_log_print(ANDROID_LOG_FATAL, "KisakCOD", "pthread_create: %s", strerror(error));
        g_engineStarted.store(false);
        return false;
    }
    return true;
}
