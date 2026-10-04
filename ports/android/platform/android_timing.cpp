// Monotonic clock services for the Android port, replacing src/universal/timing.cpp.
//
// The Windows build calibrates rdtsc against QueryPerformanceCounter at
// startup. arm64 has a direct equivalent in the architectural generic timer
// (CNTVCT_EL0 with CNTFRQ_EL0 as its frequency), which is monotonic, shared
// across cores, unaffected by frequency scaling and readable from userspace
// without a syscall. Reading it directly costs about 20 ns, against roughly
// 40 ns for clock_gettime() through the vDSO, which matters because the
// engine's profiler reads it thousands of times per frame.
//
// Vendor kernels occasionally report a nonsense CNTFRQ_EL0; the initialiser
// sanity-checks it against CLOCK_MONOTONIC and falls back when it disagrees.

#include <universal/q_shared.h>
#include <universal/timing.h>
#include <qcommon/threads.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>

namespace {

struct TimerSource
{
    // Nanoseconds per tick.
    double nanosPerTick = 1.0;
    bool useGenericTimer = false;
};

#if defined(__aarch64__)
inline std::uint64_t ReadGenericTimer()
{
    std::uint64_t value = 0;
    // isb before the read: without it the counter read can be speculated
    // ahead of the work being measured, which shows up as implausibly short
    // profiler intervals rather than as an obvious failure.
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

inline std::uint64_t ReadGenericTimerFrequency()
{
    std::uint64_t value = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}
#endif

std::uint64_t MonotonicNanos()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

const TimerSource &Source()
{
    static const TimerSource source = [] {
        TimerSource result;
#if defined(__aarch64__)
        const std::uint64_t frequency = ReadGenericTimerFrequency();
        // Every shipping arm64 part runs this between 1 and 100 MHz (19.2 MHz
        // on Qualcomm, 24 MHz on most others). Anything outside that is a
        // broken register, not a fast timer.
        if (frequency >= 1000000ull && frequency <= 100000000ull)
        {
            // Cross-check against CLOCK_MONOTONIC over a short interval. A
            // counter that drifts more than 5% from the kernel's own clock is
            // not usable as a time base.
            const std::uint64_t ticksBefore = ReadGenericTimer();
            const std::uint64_t nanosBefore = MonotonicNanos();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const std::uint64_t ticksAfter = ReadGenericTimer();
            const std::uint64_t nanosAfter = MonotonicNanos();

            const double measuredNanos = static_cast<double>(nanosAfter - nanosBefore);
            const double expectedNanos =
                static_cast<double>(ticksAfter - ticksBefore) * 1.0e9 / static_cast<double>(frequency);
            if (measuredNanos > 0.0 && expectedNanos > 0.0)
            {
                const double ratio = expectedNanos / measuredNanos;
                if (ratio > 0.95 && ratio < 1.05)
                {
                    result.useGenericTimer = true;
                    result.nanosPerTick = 1.0e9 / static_cast<double>(frequency);
                    return result;
                }
            }
        }
#endif
        result.useGenericTimer = false;
        result.nanosPerTick = 1.0; // clock_gettime already returns nanoseconds
        return result;
    }();
    return source;
}

std::uint32_t Milliseconds(std::uint64_t ticks)
{
    const TimerSource &source = Source();
    // A 128-bit intermediate keeps the engine's intentional uint32 wrap while
    // avoiding an earlier overflow in the tick product.
    const auto nanos = source.useGenericTimer
                           ? static_cast<__uint128_t>(static_cast<double>(ticks) * source.nanosPerTick)
                           : static_cast<__uint128_t>(ticks);
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(nanos / 1000000));
}

} // namespace

double msecPerRawTimerTick;
double qpc2msec;

double SecondsPerTick()
{
    return Source().nanosPerTick / 1.0e9;
}

void InitTiming()
{
    const TimerSource &source = Source();
    msecPerRawTimerTick = static_cast<double>(source.nanosPerTick) / 1000000.0;
    qpc2msec = static_cast<double>(msecPerRawTimerTick);
}

std::uint64_t Sys_ReadRawTimer()
{
#if defined(__aarch64__)
    if (Source().useGenericTimer)
        return ReadGenericTimer();
#endif
    return MonotonicNanos();
}

std::uint32_t Sys_Milliseconds()
{
    // Static initialisation gives one shared epoch even when worker threads
    // make the first calls simultaneously.
    static const auto start = Sys_ReadRawTimer();
    return Milliseconds(Sys_ReadRawTimer() - start);
}

std::uint32_t Sys_MillisecondsRaw()
{
    return Milliseconds(Sys_ReadRawTimer());
}

void Sys_Sleep(std::uint32_t milliseconds)
{
    if (milliseconds == 0)
    {
        std::this_thread::yield();
        return;
    }
    // std::this_thread::sleep_for goes through nanosleep on bionic, which
    // handles intervals longer than a second correctly, unlike usleep().
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
