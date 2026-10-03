// Win32 event objects for the Android port, replacing the CreateEvent/SetEvent
// family in src/qcommon/threads.cpp.
//
// Same semantics as ports/ios/platform/apple_events.cpp - manual-reset
// broadcast, auto-reset single-consumer signalling, coalescing repeated
// signals, zero-timeout polling and monotonic deadlines - implemented on
// std::condition_variable, which bionic backs with a futex.
//
// The one Android-specific detail: condition_variable::wait_until on a
// steady_clock deadline uses CLOCK_MONOTONIC on bionic, so the waits here are
// unaffected by the system clock jumping, which it does on a phone whenever
// NTP or the user changes it.

#include <universal/q_shared.h>
#include <qcommon/threads.h>
#include "android_platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>

namespace {

struct AndroidEvent
{
    AndroidEvent(bool manual, bool initial) : manualReset(manual), signaled(initial) {}
    std::mutex mutex;
    std::condition_variable changed;
    const bool manualReset;
    bool signaled;
    // Lets a manual-reset waiter tell "I was released by a Set" from "the
    // event happens to be clear now", so a Reset racing with a wakeup cannot
    // retract a release that was already issued.
    std::uint64_t generation = 0;
};

AndroidEvent &GetEvent(void **event)
{
    iassert(event && *event);
    return *static_cast<AndroidEvent *>(*event);
}

bool WaitEvent(AndroidEvent &event, const std::chrono::steady_clock::time_point *deadline)
{
    std::unique_lock<std::mutex> lock(event.mutex);
    const auto generation = event.generation;
    const auto ready = [&] { return event.signaled || (event.manualReset && event.generation != generation); };
    if (deadline)
    {
        if (!event.changed.wait_until(lock, *deadline, ready))
            return false;
    }
    else
    {
        event.changed.wait(lock, ready);
    }
    if (!event.manualReset)
        event.signaled = false;
    return true;
}

} // namespace

void Sys_CreateEvent(bool manualReset, bool initialState, void **event)
{
    iassert(event);
    *event = new AndroidEvent(manualReset, initialState);
}

void Sys_DestroyAndroidEvent(void **event)
{
    iassert(event);
    delete static_cast<AndroidEvent *>(*event);
    *event = nullptr;
}

void Sys_SetEvent(void **handle)
{
    // Win32 SetEvent/ResetEvent/WaitForSingleObject fail immediately on a null
    // handle; engine code relies on that before some events are created.
    if (!handle || !*handle)
        return;
    auto &event = GetEvent(handle);
    std::lock_guard<std::mutex> lock(event.mutex);
    event.signaled = true;
    if (event.manualReset)
    {
        ++event.generation;
        event.changed.notify_all();
    }
    else
    {
        // Repeated Sets coalesce while signalled, as Win32 events do.
        event.changed.notify_one();
    }
}

void Sys_ResetEvent(void **handle)
{
    if (!handle || !*handle)
        return;
    auto &event = GetEvent(handle);
    std::lock_guard<std::mutex> lock(event.mutex);
    event.signaled = false;
}

void Sys_WaitForSingleObject(void **handle)
{
    if (!handle || !*handle)
        return;
    WaitEvent(GetEvent(handle), nullptr);
}

bool Sys_WaitForSingleObjectTimeout(void **handle, std::uint32_t milliseconds)
{
    if (!handle || !*handle)
        return false;
    // The engine reserves UINT32_MAX for its non-timed wait API.
    iassert(milliseconds != std::numeric_limits<std::uint32_t>::max());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    return WaitEvent(GetEvent(handle), &deadline);
}
