#pragma once

// CPU cluster discovery and thread placement for Android.
//
// Every modern phone SoC is heterogeneous: a prime core, a few big cores and
// four or more little ones, with per-cluster clocks that differ by a factor of
// two or more. The Linux scheduler on Android is tuned for responsiveness and
// battery life, not for a 16.6 ms deadline, so it will happily migrate a
// render thread onto a little core mid-frame. That single migration costs more
// than any shader optimisation will ever win back.
//
// This component reads the cluster layout out of sysfs, groups cores by
// capacity, and produces an affinity plan: latency-critical threads are pinned
// to the fastest cluster, throughput workers to the big cores, and background
// work to the little cores where it neither steals a big core nor adds heat.
//
// The parsing is separated from the filesystem so it can be driven from
// fixtures on the host; ports/android/tests/CpuTopologyTests.cpp does exactly
// that with layouts taken from common SoC families.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kisak::perf {

enum class CoreClass : uint8_t
{
    Little,
    Big,
    Prime,
};

struct CpuCore
{
    int id = 0;
    // kHz, as reported by cpufreq.
    int64_t maxFrequencyKhz = 0;
    // Scheduler capacity (0..1024) when the kernel exposes it. More reliable
    // than frequency alone, because two clusters at the same clock can differ
    // substantially in IPC.
    int capacity = 0;
    CoreClass cls = CoreClass::Little;
};

// Role a thread plays, which decides where it is allowed to run.
enum class ThreadRole : uint8_t
{
    // Simulation plus render command submission. The deadline thread.
    Render,
    // Parallel engine work: skinning, particles, visibility.
    Worker,
    // Audio mixing. Small, periodic, latency-sensitive but cheap.
    Audio,
    // Streaming, fastfile decompression, downloads. Throughput, no deadline.
    Background,
};

struct CpuTopology
{
    std::vector<CpuCore> cores;
    std::vector<int> little;
    std::vector<int> big;
    std::vector<int> prime;

    bool heterogeneous() const { return !big.empty() || !prime.empty(); }
    std::size_t coreCount() const { return cores.size(); }

    // CPU ids a thread in this role should be restricted to. Falls back to
    // "everything" when the layout is unknown or uniform, so the plan is
    // always safe to apply.
    std::vector<int> AffinityFor(ThreadRole role) const;
};

// Reads one sysfs file. Returns false when it does not exist. Injected so the
// parser can be tested without a device.
using SysfsReader = std::function<bool(const std::string &path, std::string &out)>;

// Default reader, backed by the real filesystem.
bool ReadSysfsFile(const std::string &path, std::string &out);

// Discovers the layout. `coreCount` is normally sysconf(_SC_NPROCESSORS_CONF).
CpuTopology DiscoverTopology(int coreCount, const SysfsReader &reader = ReadSysfsFile);

// Pins the calling thread to the cores for `role`. No-op (returning false)
// when the platform refuses, which some vendor kernels do for non-system apps;
// the engine must keep working in that case.
bool ApplyAffinity(const CpuTopology &topology, ThreadRole role);

// Asks the kernel to treat this thread as latency-sensitive. Uses the
// Android Performance Hint API (API 33+) when present and falls back to a
// nice() adjustment, which is all that is available on older releases.
bool RequestHighPriority(ThreadRole role);

} // namespace kisak::perf
