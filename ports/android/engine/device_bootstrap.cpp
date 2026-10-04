// Builds the DeviceDescriptor the performance subsystem reasons about, and
// hands it to the director.
//
// This is the join between three sources that each know part of the answer:
//
//   /proc and system properties  CPU clusters, clock ceilings, RAM, SoC name
//   the activity                 display size and refresh rate
//   the Vulkan backend           GPU name, vendor, API level, texture formats
//
// The awkward part is ordering. The GPU is not created until the renderer
// initialises, which happens inside Com_Init - well after the engine needs a
// quality profile to configure itself with. So this runs twice:
//
//   Bootstrap()  before Com_Init, with everything except the GPU. Enough for
//                thread placement, the frame pacer and a provisional tier
//                based on RAM, cores and clocks.
//   Refresh()    after Com_Init, once the Vulkan device exists. Reclassifies
//                with the GPU model, which is the strongest single signal,
//                and the result is what gets applied to the console.
//
// Running it only once in either position would be wrong: before, and the GPU
// is unknown so an Adreno 740 and an Adreno 610 with the same RAM get the
// same settings; after, and the pacer spent the whole of Com_Init - which is
// several seconds of fastfile loading - with no idea what it was running on.

#include "../perf/perf_director.h"
#include "../perf/device_profile.h"
#include "../platform/android_platform.h"
#include "../gfx/gpu_backend.h"

#include <android/log.h>

#include "../platform/android_log.h"
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define LOGI(...) KISAK_LOGI("KisakCOD-perf", __VA_ARGS__)

namespace {

std::string SystemProperty(const char *name)
{
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get(name, value);
    return length > 0 ? std::string(value, static_cast<std::size_t>(length)) : std::string();
}

// First non-empty of several properties. Vendors disagree about which one
// carries the SoC name: Qualcomm fills ro.soc.model, MediaTek historically
// only ro.board.platform, and some Samsung builds only ro.hardware.
std::string FirstProperty(std::initializer_list<const char *> names)
{
    for (const char *name : names)
    {
        std::string value = SystemProperty(name);
        if (!value.empty())
            return value;
    }
    return {};
}

uint32_t ReadTotalRamMB()
{
    FILE *file = std::fopen("/proc/meminfo", "re");
    if (!file)
        return 0;
    char line[256];
    uint32_t megabytes = 0;
    while (std::fgets(line, sizeof(line), file))
    {
        unsigned long kilobytes = 0;
        if (std::sscanf(line, "MemTotal: %lu kB", &kilobytes) == 1)
        {
            megabytes = static_cast<uint32_t>(kilobytes / 1024);
            break;
        }
    }
    std::fclose(file);
    return megabytes;
}

// Highest cpufreq ceiling across all cores, in kHz. Used as a tie-breaker:
// a part that tops out below 1.9 GHz is a budget chip whatever its GPU says.
int64_t ReadMaxFrequencyKhz(int coreCount)
{
    int64_t best = 0;
    for (int core = 0; core < coreCount; ++core)
    {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", core);
        FILE *file = std::fopen(path, "re");
        if (!file)
            continue;
        long long value = 0;
        if (std::fscanf(file, "%lld", &value) == 1 && value > best)
            best = value;
        std::fclose(file);
    }
    return best;
}

kisak::perf::DeviceDescriptor Describe(bool includeGpu)
{
    kisak::perf::DeviceDescriptor device;

    device.soc = FirstProperty({ "ro.soc.model", "ro.board.platform", "ro.hardware" });
    device.deviceModel = FirstProperty({ "ro.product.model", "ro.product.device" });
    device.gpuVendor = FirstProperty({ "ro.soc.manufacturer" });
    device.totalRamMB = ReadTotalRamMB();

    // The topology has already walked /sys by the time this is called the
    // second time; the first time it is built here.
    const kisak::perf::CpuTopology &topology = kisak::perf::Director().topology();
    device.totalCores = static_cast<int>(topology.coreCount());
    device.bigCores = static_cast<int>(topology.big.size() + topology.prime.size());
    device.maxFrequencyKhz = ReadMaxFrequencyKhz(device.totalCores > 0 ? device.totalCores : 8);

    int width = 0;
    int height = 0;
    KisakAndroid_GetDisplaySize(&width, &height);
    device.displayWidth = static_cast<uint32_t>(width > 0 ? width : 1920);
    device.displayHeight = static_cast<uint32_t>(height > 0 ? height : 1080);
    device.displayHz = KisakAndroid_GetDisplayRefreshRate();

    if (includeGpu && kisak::vk::Available())
    {
        const kisak::vk::DeviceInfo &info = kisak::vk::GetDeviceInfo();
        // VkPhysicalDeviceProperties::deviceName is what ClassifyGpu matches
        // on - "Adreno (TM) 730", "Mali-G78 MP14", "Immortalis-G715" and so
        // on. It is the same string GL_RENDERER gives, which is what the
        // classifier's tables were written against.
        device.gpuRenderer = info.deviceName ? info.deviceName : "";
        if (device.gpuVendor.empty() && info.driverName)
            device.gpuVendor = info.driverName;
        device.vulkanApiVersion = info.apiVersion;
        device.supportsAstc = info.supportsAstc;
        device.supportsEtc2 = info.supportsEtc2;
        device.immediateModeRenderer = info.immediateMode;
    }
    else
    {
        // No GPU string yet. Leaving it empty makes ClassifyGpu return the
        // Unknown tier, which maps to Medium - recoverable in both
        // directions once Refresh() runs.
        device.gpuRenderer.clear();
        device.vulkanApiVersion = 0;
    }

    return device;
}

bool g_bootstrapped = false;

} // namespace

// Called before Com_Init. Everything except the GPU.
void KisakAndroid_BootstrapDeviceProfile()
{
    if (g_bootstrapped)
        return;
    g_bootstrapped = true;

    const kisak::perf::DeviceDescriptor device = Describe(false);
    const kisak::perf::QualityProfile profile = kisak::perf::ProfileForDevice(device);

    kisak::perf::PerfDirector &director = kisak::perf::Director();
    director.Initialise(device, profile);
    director.SetNativeResolution(device.displayWidth, device.displayHeight);
    director.SetDisplayRefreshRate(device.displayHz);

    LOGI("provisional profile: %s, %ux%u @ %.1f Hz, %u MB, %d cores (%d big), %lld kHz",
         kisak::perf::QualityTierName(profile.tier), device.displayWidth, device.displayHeight,
         device.displayHz, device.totalRamMB, device.totalCores, device.bigCores,
         static_cast<long long>(device.maxFrequencyKhz));
}

// Called after Com_Init, once the Vulkan device exists. Returns the profile
// to apply to the console.
const kisak::perf::QualityProfile &KisakAndroid_RefreshDeviceProfile()
{
    kisak::perf::PerfDirector &director = kisak::perf::Director();

    if (!kisak::vk::Available())
    {
        // Headless, or Vulkan failed. The provisional profile stands; it was
        // built from RAM, cores and clocks, which is not nothing.
        LOGI("no Vulkan device; keeping the provisional profile");
        return director.profile();
    }

    const kisak::perf::DeviceDescriptor device = Describe(true);
    const kisak::perf::QualityProfile profile = kisak::perf::ProfileForDevice(device);

    director.Initialise(device, profile);
    director.SetNativeResolution(device.displayWidth, device.displayHeight);
    director.SetDisplayRefreshRate(device.displayHz);

    LOGI("device profile: %s on %s (%s), render scale %.2f, target %d fps",
         kisak::perf::QualityTierName(profile.tier),
         device.gpuRenderer.empty() ? "unknown GPU" : device.gpuRenderer.c_str(),
         device.soc.empty() ? "unknown SoC" : device.soc.c_str(), profile.renderScale, profile.targetFps);

    return director.profile();
}
