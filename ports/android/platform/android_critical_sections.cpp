// Critical sections for the Android port, replacing the Win32 CRITICAL_SECTION
// bodies in src/qcommon/threads.cpp.
//
// Win32 critical sections are recursive and spin briefly before blocking. A
// plain std::recursive_mutex on bionic goes straight to a futex wait, and the
// engine takes CRITSECT_DEVGUI and CRITSECT_CONSOLE tens of thousands of times
// per frame with contention measured in tens of nanoseconds. On a phone that
// syscall traffic is not free, so these spin first, like the original did.
//
// The spin count is deliberately small. Long spins on a little core burn the
// thermal budget the frame pacer is trying to protect.

#include <universal/q_shared.h>
#include <qcommon/critical_sections.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

namespace {

class SpinThenBlock
{
public:
    void lock()
    {
        const std::thread::id self = std::this_thread::get_id();
        if (m_depth > 0 && m_owner == self)
        {
            ++m_depth;
            return;
        }
        // Roughly a microsecond of spinning on a big core. Past that the
        // holder is doing real work and blocking is cheaper than burning
        // clocks next to it.
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            if (m_mutex.try_lock())
            {
                m_owner = self;
                m_depth = 1;
                return;
            }
            // yield is the architectural hint that this core is in a spin
            // loop; it lets SMT siblings and the DVFS governor react.
            asm volatile("yield" ::: "memory");
        }
        m_mutex.lock();
        m_owner = self;
        m_depth = 1;
    }

    void unlock()
    {
        iassert(m_depth > 0);
        if (--m_depth == 0)
        {
            m_owner = std::thread::id();
            m_mutex.unlock();
        }
    }

private:
    std::mutex m_mutex;
    // Only ever read or written by the lock holder, so no atomics needed:
    // a thread that does not hold the lock cannot observe a matching id.
    std::thread::id m_owner;
    int m_depth = 0;
};

auto &Sections()
{
    static std::array<SpinThenBlock, CRITSECT_COUNT> locks;
    return locks;
}

} // namespace

void Sys_InitializeCriticalSections()
{
    (void)Sections();
}

void Sys_EnterCriticalSection(int section)
{
    iassert(section >= 0 && section < CRITSECT_COUNT);
    Sections()[section].lock();
}

void Sys_LeaveCriticalSection(int section)
{
    iassert(section >= 0 && section < CRITSECT_COUNT);
    Sections()[section].unlock();
}
