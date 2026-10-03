#include "android_log.h"

#include "android_platform.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

#define KISAK_LOG_TAG "KisakCOD"

namespace {

constexpr size_t kPathMax = 1024;
constexpr int kMaxFrames = 64;

char g_path[kPathMax] = "";
std::atomic<int> g_fd{-1};
std::mutex g_mutex;

// The signals that mean the process is going down. SIGABRT is in the list
// because Sys_Error() calls abort() deliberately, and a backtrace of that is
// just as useful as one from a segfault.
const int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS};
constexpr size_t kFatalSignalCount = sizeof(kFatalSignals) / sizeof(kFatalSignals[0]);
struct sigaction g_previousAction[kFatalSignalCount];

// A stack overflow arrives as SIGSEGV on a thread whose stack is already
// exhausted, so the handler needs somewhere else to run. The engine recurses
// deeply through the BSP and the script VM, which makes that a likely crash
// rather than a theoretical one.
//
// sigaltstack is per thread, unlike sigaction. Installing one on the thread
// that happened to call InstallCrashHandler would leave the engine thread -
// the one that actually overflows - without a stack to report from, so this
// runs once per thread and the memory is deliberately never freed: it has to
// outlive every thread that might fault on it.
void InstallAlternateStack()
{
    stack_t existing;
    std::memset(&existing, 0, sizeof(existing));
    if (sigaltstack(nullptr, &existing) == 0 && existing.ss_sp != nullptr && (existing.ss_flags & SS_DISABLE) == 0)
        return;

    const size_t size = static_cast<size_t>(SIGSTKSZ) + 64 * 1024;
    void *const memory = std::malloc(size);
    if (memory == nullptr)
        return;

    stack_t alternate;
    std::memset(&alternate, 0, sizeof(alternate));
    alternate.ss_sp = memory;
    alternate.ss_size = size;
    alternate.ss_flags = 0;
    if (sigaltstack(&alternate, nullptr) != 0)
        std::free(memory);
}

// ---------------------------------------------------------------------------
// Async-signal-safe output primitives.
//
// No stdio below this line: a signal can arrive in the middle of an fwrite(),
// and re-entering stdio from the handler deadlocks on its internal lock.

void WriteAll(int fd, const char *data, size_t length)
{
    while (length > 0)
    {
        const ssize_t written = write(fd, data, length);
        if (written <= 0)
        {
            if (written < 0 && errno == EINTR)
                continue;
            return;
        }
        data += static_cast<size_t>(written);
        length -= static_cast<size_t>(written);
    }
}

void RawWrite(const char *text, size_t length)
{
    const int fd = g_fd.load(std::memory_order_relaxed);
    if (fd >= 0 && length > 0)
        WriteAll(fd, text, length);
}

void RawWrite(const char *text)
{
    if (text)
        RawWrite(text, std::strlen(text));
}

void RawWriteNumber(unsigned long long value, unsigned base, unsigned pad)
{
    char digits[32];
    size_t index = sizeof(digits);
    do
    {
        const unsigned digit = static_cast<unsigned>(value % base);
        digits[--index] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= base;
    } while (value != 0 && index > 0);
    while (sizeof(digits) - index < pad && index > 0)
        digits[--index] = '0';
    RawWrite(digits + index, sizeof(digits) - index);
}

void RawWriteSigned(long long value)
{
    if (value < 0)
    {
        RawWrite("-", 1);
        value = -value;
    }
    RawWriteNumber(static_cast<unsigned long long>(value), 10, 0);
}

void RawWritePointer(const void *pointer)
{
    RawWrite("0x", 2);
    RawWriteNumber(reinterpret_cast<uintptr_t>(pointer), 16, 0);
}

// ---------------------------------------------------------------------------
// Backtrace
//
// bionic has no <execinfo.h>, so the unwinder is driven directly. Symbol names
// are usually absent because release libraries are stripped; the library name
// plus the offset within it is the part that matters, because that is what
// llvm-symbolizer needs to turn a crash into source lines against the
// unstripped .so kept by the build.

struct BacktraceState
{
    void **current;
    void **end;
};

_Unwind_Reason_Code UnwindFrame(_Unwind_Context *context, void *argument)
{
    BacktraceState *const state = static_cast<BacktraceState *>(argument);
    const uintptr_t pc = _Unwind_GetIP(context);
    if (pc != 0)
    {
        if (state->current == state->end)
            return _URC_END_OF_STACK;
        *state->current++ = reinterpret_cast<void *>(pc);
    }
    return _URC_NO_REASON;
}

void WriteBacktrace()
{
    void *frames[kMaxFrames];
    BacktraceState state = {frames, frames + kMaxFrames};
    _Unwind_Backtrace(&UnwindFrame, &state);

    const size_t count = static_cast<size_t>(state.current - frames);
    if (count == 0)
    {
        RawWrite("  (the unwinder returned no frames)\n");
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        RawWrite("  #");
        RawWriteNumber(i, 10, 2);
        RawWrite("  ");
        RawWritePointer(frames[i]);

        Dl_info info;
        std::memset(&info, 0, sizeof(info));
        if (dladdr(frames[i], &info) != 0 && info.dli_fname != nullptr)
        {
            RawWrite("  ");
            RawWrite(info.dli_fname);
            if (info.dli_fbase != nullptr)
            {
                // The offset a symboliser wants: address minus the load
                // address of the library, not minus the nearest symbol.
                RawWrite("+0x");
                RawWriteNumber(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase), 16, 0);
            }
            if (info.dli_sname != nullptr)
            {
                RawWrite("  ");
                RawWrite(info.dli_sname);
            }
        }
        RawWrite("\n", 1);
    }
}

// ---------------------------------------------------------------------------
// Crash handler

const char *SignalName(int signalNumber)
{
    switch (signalNumber)
    {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (misaligned or unmapped access)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort - an engine error or a failed assertion)";
    case SIGTRAP: return "SIGTRAP (trap)";
    case SIGSYS: return "SIGSYS (bad system call)";
    default: return "signal";
    }
}

void RestoreDefaultHandlers()
{
    for (size_t i = 0; i < kFatalSignalCount; ++i)
        sigaction(kFatalSignals[i], &g_previousAction[i], nullptr);
}

void CrashHandler(int signalNumber, siginfo_t *info, void *context)
{
    (void)context;

    // Two threads can fault at once, and the handler itself can fault while
    // unwinding a corrupt stack. Either way only the first report is wanted;
    // a second pass would overwrite a good report with a worse one.
    static std::atomic<bool> reporting{false};
    bool notReporting = false;
    if (reporting.compare_exchange_strong(notReporting, true))
    {
        char threadName[20];
        std::memset(threadName, 0, sizeof(threadName));
        prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(threadName), 0UL, 0UL, 0UL);

        RawWrite("\n==== CRASH ====\n");
        RawWrite("signal:  ");
        RawWrite(SignalName(signalNumber));
        RawWrite("\n");
        if (info != nullptr)
        {
            RawWrite("code:    ");
            RawWriteSigned(info->si_code);
            RawWrite("\naddress: ");
            RawWritePointer(info->si_addr);
            RawWrite("\n");
        }
        RawWrite("thread:  ");
        RawWrite(threadName[0] ? threadName : "(unnamed)");
        RawWrite(" tid ");
        RawWriteSigned(gettid());
        RawWrite("\nbacktrace:\n");
        WriteBacktrace();
        RawWrite("==== END CRASH ====\n");

        const int fd = g_fd.load(std::memory_order_relaxed);
        if (fd >= 0)
            fsync(fd);

        __android_log_write(ANDROID_LOG_FATAL, KISAK_LOG_TAG, "crash report appended to the log file");
    }

    // Hand the signal back to whoever had it before - normally the runtime's
    // own handler, which writes the tombstone and shows the system dialog.
    // Suppressing that would trade a diagnosable crash for a silent one.
    RestoreDefaultHandlers();
    raise(signalNumber);
}

void TerminateHandler()
{
    RawWrite("\n==== UNCAUGHT C++ EXCEPTION ====\n");
    if (std::exception_ptr pending = std::current_exception())
    {
        try
        {
            std::rethrow_exception(pending);
        }
        catch (const std::exception &error)
        {
            RawWrite("what: ");
            RawWrite(error.what());
            RawWrite("\n");
        }
        catch (...)
        {
            RawWrite("a non-standard exception was thrown\n");
        }
    }
    WriteBacktrace();
    KisakAndroid_LogFlush();
    // SIGABRT is handled above, so the abort also records where it came from.
    std::abort();
}

// ---------------------------------------------------------------------------
// Header

std::string ReadProperty(const char *name)
{
    char value[PROP_VALUE_MAX];
    const int length = __system_property_get(name, value);
    return length > 0 ? std::string(value, static_cast<size_t>(length)) : std::string();
}

void WriteHeader()
{
    char line[512];
    std::time_t now = std::time(nullptr);
    std::tm local;
    char stamp[64] = "unknown";
    if (localtime_r(&now, &local) != nullptr)
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

    const std::string manufacturer = ReadProperty("ro.product.manufacturer");
    const std::string model = ReadProperty("ro.product.model");
    const std::string release = ReadProperty("ro.build.version.release");
    const std::string sdk = ReadProperty("ro.build.version.sdk");
    const std::string soc = ReadProperty("ro.soc.model");

    std::snprintf(line, sizeof(line),
        "==== KisakCOD ====\n"
        "started:   %s\n"
        "device:    %s %s\n"
        "android:   %s (API %s)\n"
        "soc:       %s\n"
        "game data: %s\n"
        "\n",
        stamp,
        manufacturer.empty() ? "?" : manufacturer.c_str(),
        model.empty() ? "?" : model.c_str(),
        release.empty() ? "?" : release.c_str(),
        sdk.empty() ? "?" : sdk.c_str(),
        soc.empty() ? "?" : soc.c_str(),
        KisakAndroid_GameDataPath());
    RawWrite(line);
}

} // namespace

// ---------------------------------------------------------------------------

void KisakAndroid_LogSetPath(const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (path && *path)
    {
        std::snprintf(g_path, sizeof(g_path), "%s", path);
    }
}

const char *KisakAndroid_LogPath()
{
    return g_path;
}

void KisakAndroid_LogOpen()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_fd.load(std::memory_order_relaxed) >= 0)
        return;

    if (!g_path[0])
    {
        const char *root = KisakAndroid_PrivatePath();
        if (!root || !*root)
            return;
        std::snprintf(g_path, sizeof(g_path), "%s/kisakcod-log.txt", root);
    }

    // O_APPEND rather than O_TRUNC: the launcher rotates the file before the
    // engine starts, and both engine libraries write to the same name.
    const int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, KISAK_LOG_TAG, "could not open log '%s': %s", g_path, std::strerror(errno));
        return;
    }
    g_fd.store(fd, std::memory_order_relaxed);
    WriteHeader();
}

void KisakAndroid_LogWrite(const char *text)
{
    if (!text || !*text)
        return;
    if (g_fd.load(std::memory_order_relaxed) < 0)
        KisakAndroid_LogOpen();
    RawWrite(text);
}

void KisakAndroid_LogPrintf(const char *format, ...)
{
    char message[4096];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    KisakAndroid_LogWrite(message);
}

void KisakAndroid_LogFlush()
{
    // Nothing is buffered in user space, so this only has to push the page
    // cache out - which is what makes the tail survive a kill -9 from the
    // low memory killer as well as a crash.
    const int fd = g_fd.load(std::memory_order_relaxed);
    if (fd >= 0)
        fsync(fd);
}

void KisakAndroid_InstallCrashHandler()
{
    // Per thread, so this part runs on every call.
    InstallAlternateStack();

    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true))
        return;

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    for (size_t i = 0; i < kFatalSignalCount; ++i)
        sigaction(kFatalSignals[i], &action, &g_previousAction[i]);

    std::set_terminate(&TerminateHandler);
}
