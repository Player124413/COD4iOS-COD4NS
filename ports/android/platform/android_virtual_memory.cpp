// Reserve/commit virtual memory, replacing the VirtualAlloc calls the engine's
// hunk allocator makes (src/universal/com_memory.cpp).
//
// Mirrors ports/ios/platform/apple_virtual_memory.cpp, with two Android
// differences that are not cosmetic:
//
//   * Pages are 4 KB here (16 KB on Apple arm64, and 16 KB on Android 15+
//     devices that opt into it). The engine's callers work in 4 KB units, so
//     decommit still has to round inwards to avoid releasing a neighbour's
//     live data on a 16 KB kernel.
//   * Android charges PROT_NONE reservations against the process's virtual
//     address space but not its RSS, while the low-memory killer scores on
//     RSS. Naming the mappings makes a kill decision auditable in
//     /proc/<pid>/maps and in the tombstone, which is the only way to tell a
//     hunk overflow from a driver leak after the fact.

#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <map>
#include <mutex>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

namespace {

struct Reservation
{
    std::size_t bytes;
};

std::mutex reservationMutex;
std::map<std::uintptr_t, Reservation> reservations;

std::uintptr_t floorPage(std::uintptr_t address) { return address & ~(Z_VirtualPageSize() - 1); }
std::uintptr_t ceilPage(std::uintptr_t address) { return floorPage(address + Z_VirtualPageSize() - 1); }

void verifyRange(std::uintptr_t begin, std::size_t size)
{
    auto next = reservations.upper_bound(begin);
    if (next != reservations.begin())
    {
        const auto &entry = *std::prev(next);
        const auto offset = begin - entry.first;
        if (offset <= entry.second.bytes && size <= entry.second.bytes - offset)
            return;
    }
    Com_Error(ERR_FATAL, "Virtual memory range lies outside its reservation");
}

// Shows up as "[anon:kisakcod-hunk]" in /proc/<pid>/maps and in tombstones.
// Ignored on kernels without the prctl, which is why the result is unchecked.
void NameMapping(void *address, std::size_t bytes)
{
    prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, address, bytes, "kisakcod-hunk");
}

} // namespace

std::size_t Z_VirtualPageSize()
{
    static const auto pageSize = [] {
        const auto value = sysconf(_SC_PAGESIZE);
        iassert(value > 0 && !(value & (value - 1)));
        return std::size_t(value);
    }();
    return pageSize;
}

void *Z_VirtualReserve(int size)
{
    iassert(size > 0);
    const auto bytes = ceilPage(std::size_t(size));
    void *result = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (result == MAP_FAILED)
    {
        Com_Error(ERR_FATAL, "Virtual memory reservation failed: %d", errno);
        return nullptr;
    }
    NameMapping(result, bytes);
    std::lock_guard<std::mutex> lock(reservationMutex);
    try
    {
        reservations.emplace(reinterpret_cast<std::uintptr_t>(result), Reservation{bytes});
    }
    catch (...)
    {
        munmap(result, bytes);
        throw;
    }
    return result;
}

void Z_VirtualCommit(void *ptr, int size)
{
    iassert(size >= 0);
    if (!size)
        return;
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto begin = floorPage(address);
    const auto bytes = ceilPage(address + std::size_t(size)) - begin;
    std::lock_guard<std::mutex> lock(reservationMutex);
    verifyRange(begin, bytes);
    if (mprotect(reinterpret_cast<void *>(begin), bytes, PROT_READ | PROT_WRITE) != 0)
        Com_Error(ERR_FATAL, "Virtual memory commit failed: %d", errno);
}

void Z_VirtualDecommit(void *ptr, int size)
{
    iassert(size >= 0);
    if (!size)
        return;
    // Round inwards: callers work in 4 KB units but the kernel page may be
    // larger, and releasing a partially covered page would take a neighbour's
    // live data with it.
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto begin = ceilPage(address);
    const auto end = floorPage(address + std::size_t(size));
    if (end <= begin)
        return;
    const auto bytes = end - begin;
    std::lock_guard<std::mutex> lock(reservationMutex);
    verifyRange(begin, bytes);
    // Replace the pages rather than madvise(MADV_DONTNEED): the engine relies
    // on a recommitted range reading back as zero, and only a fresh anonymous
    // mapping guarantees that on every kernel.
    void *replaced = mmap(reinterpret_cast<void *>(begin), bytes, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (replaced == MAP_FAILED)
    {
        Com_Error(ERR_FATAL, "Virtual memory decommit failed: %d", errno);
        return;
    }
    NameMapping(replaced, bytes);
}

void Z_VirtualFree(void *ptr)
{
    if (!ptr)
        return;
    std::lock_guard<std::mutex> lock(reservationMutex);
    const auto entry = reservations.find(reinterpret_cast<std::uintptr_t>(ptr));
    if (entry == reservations.end())
    {
        Com_Error(ERR_FATAL, "Virtual memory free requires a reservation base");
        return;
    }
    if (munmap(ptr, entry->second.bytes) != 0)
    {
        Com_Error(ERR_FATAL, "Virtual memory release failed: %d", errno);
        return;
    }
    reservations.erase(entry);
}

// com_memory.cpp builds its allocation helpers on these primitives.
bool Z_TryVirtualCommitInternal(void *ptr, int size)
{
    iassert(size >= 0);
    if (!size)
        return true;
    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    const auto begin = floorPage(address);
    const auto bytes = ceilPage(address + std::size_t(size)) - begin;
    std::lock_guard<std::mutex> lock(reservationMutex);
    verifyRange(begin, bytes);
    return mprotect(reinterpret_cast<void *>(begin), bytes, PROT_READ | PROT_WRITE) == 0;
}

void Z_VirtualCommitInternal(void *ptr, int size) { Z_VirtualCommit(ptr, size); }
void Z_VirtualDecommitInternal(void *ptr, int size) { Z_VirtualDecommit(ptr, size); }
void Z_VirtualFreeInternal(void *ptr) { Z_VirtualFree(ptr); }
