#include "cpu_topology.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#if defined(__ANDROID__) || defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace kisak::perf {

namespace {

int64_t ParseInt(const std::string &text)
{
    errno = 0;
    char *end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || errno != 0 || value < 0)
        return 0;
    return static_cast<int64_t>(value);
}

std::string Path(const char *format, int cpu)
{
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), format, cpu);
    return std::string(buffer);
}

} // namespace

bool ReadSysfsFile(const std::string &path, std::string &out)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    char buffer[256];
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read] = 0;
    out.assign(buffer, read);
    // sysfs values carry a trailing newline that strtoll tolerates but
    // comparisons do not.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return true;
}

CpuTopology DiscoverTopology(int coreCount, const SysfsReader &reader)
{
    CpuTopology topology;
    if (coreCount <= 0)
        coreCount = 1;
    coreCount = std::min(coreCount, 64);

    for (int cpu = 0; cpu < coreCount; ++cpu)
    {
        CpuCore core;
        core.id = cpu;

        std::string value;
        if (reader(Path("/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu), value))
            core.maxFrequencyKhz = ParseInt(value);
        // Some kernels only populate the policy node for the cluster leader.
        if (core.maxFrequencyKhz == 0 &&
            reader(Path("/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", cpu), value))
            core.maxFrequencyKhz = ParseInt(value);
        if (reader(Path("/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu), value))
            core.capacity = static_cast<int>(ParseInt(value));

        topology.cores.push_back(core);
    }

    // Rank clusters by capacity when the kernel reports it, otherwise by
    // clock. Capacity is the better signal: it already accounts for the IPC
    // difference between a Cortex-A5xx and a Cortex-X core at equal clocks.
    const bool haveCapacity =
        std::any_of(topology.cores.begin(), topology.cores.end(), [](const CpuCore &c) { return c.capacity > 0; });

    auto rankOf = [haveCapacity](const CpuCore &core) -> int64_t {
        return haveCapacity ? core.capacity : core.maxFrequencyKhz;
    };

    std::map<int64_t, std::vector<int>> clusters;
    for (const CpuCore &core : topology.cores)
        clusters[rankOf(core)].push_back(core.id);

    // One rank means a uniform machine (or a kernel that hides the layout):
    // treat every core as big so nothing gets exiled to a slow cluster that
    // may not exist.
    if (clusters.size() <= 1)
    {
        for (CpuCore &core : topology.cores)
        {
            core.cls = CoreClass::Big;
            topology.big.push_back(core.id);
        }
        return topology;
    }

    // Highest rank is the prime cluster, lowest is little, anything between is
    // big. Two-cluster parts (4+4) have no prime tier, so their top cluster is
    // big and AffinityFor() falls back accordingly.
    auto highest = std::prev(clusters.end());
    auto lowest = clusters.begin();
    const bool hasPrimeTier = clusters.size() >= 3 || highest->second.size() <= 2;

    for (auto it = clusters.begin(); it != clusters.end(); ++it)
    {
        CoreClass cls = CoreClass::Big;
        if (it == lowest)
            cls = CoreClass::Little;
        else if (it == highest && hasPrimeTier)
            cls = CoreClass::Prime;
        else if (it == highest)
            cls = CoreClass::Big;

        for (const int id : it->second)
        {
            topology.cores[static_cast<std::size_t>(id)].cls = cls;
            switch (cls)
            {
            case CoreClass::Little: topology.little.push_back(id); break;
            case CoreClass::Big: topology.big.push_back(id); break;
            case CoreClass::Prime: topology.prime.push_back(id); break;
            }
        }
    }

    return topology;
}

std::vector<int> CpuTopology::AffinityFor(ThreadRole role) const
{
    std::vector<int> all;
    all.reserve(cores.size());
    for (const CpuCore &core : cores)
        all.push_back(core.id);

    if (!heterogeneous())
        return all;

    switch (role)
    {
    case ThreadRole::Render:
    {
        // The deadline thread gets the prime core plus the big cluster. Prime
        // alone would be faster per frame but throttles first and leaves no
        // core to migrate to when it does.
        std::vector<int> result = prime;
        result.insert(result.end(), big.begin(), big.end());
        return result.empty() ? all : result;
    }
    case ThreadRole::Worker:
    {
        // Workers stay off the prime core so they cannot preempt the render
        // thread on the one core it most wants.
        std::vector<int> result = big;
        if (result.empty())
            result = prime;
        if (result.empty())
            result = all;
        return result;
    }
    case ThreadRole::Audio:
    {
        // Mixing is a few hundred microseconds per buffer. A little core runs
        // it comfortably and never competes with rendering.
        std::vector<int> result = little;
        return result.empty() ? all : result;
    }
    case ThreadRole::Background:
    {
        std::vector<int> result = little;
        return result.empty() ? all : result;
    }
    }
    return all;
}

bool ApplyAffinity(const CpuTopology &topology, ThreadRole role)
{
#if defined(__ANDROID__) || defined(__linux__)
    const std::vector<int> cpus = topology.AffinityFor(role);
    if (cpus.empty() || cpus.size() == topology.coreCount())
        return true; // nothing to restrict

    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int cpu : cpus)
    {
        if (cpu >= 0 && cpu < CPU_SETSIZE)
            CPU_SET(cpu, &set);
    }
    // sched_setaffinity on the calling thread: pid 0 means "this thread" for
    // the syscall, unlike the pthread wrapper which takes a thread handle.
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)topology;
    (void)role;
    return false;
#endif
}

bool RequestHighPriority(ThreadRole role)
{
#if defined(__ANDROID__) || defined(__linux__)
    // Android maps thread priority onto nice values and cgroup placement.
    // -10 is what the platform gives its own UI thread; going lower requires
    // privileges a sideloaded app does not have.
    int niceValue = 0;
    switch (role)
    {
    case ThreadRole::Render: niceValue = -10; break;
    case ThreadRole::Audio: niceValue = -16; break;
    case ThreadRole::Worker: niceValue = -4; break;
    case ThreadRole::Background: niceValue = 5; break;
    }
    errno = 0;
    const int tid = static_cast<int>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), niceValue) == 0 && errno == 0)
        return true;
    // Some kernels refuse negative nice for app UIDs. Settle for neutral
    // rather than leaving the thread wherever the parent left it.
    errno = 0;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 0) == 0 && errno == 0;
#else
    (void)role;
    return false;
#endif
}

} // namespace kisak::perf
