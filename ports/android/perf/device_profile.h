#pragma once

// Automatic quality selection from the device's hardware.
//
// The Windows engine benchmarks the CPU at startup to pick default detail
// settings (see Sys_FindInfo in src/win32). That benchmark assumes an x86
// desktop and produces nonsense on a phone, so the Apple port replaced it with
// a fixed rating. Android cannot do the same: the range between the cheapest
// 64-bit phone that will run this and a current flagship is roughly fifteen
// times the GPU throughput.
//
// This classifier maps the GPU renderer string, SoC identity, memory and core
// layout onto a tier, and each tier onto a concrete set of engine dvars. The
// player can override everything in the launcher; this only decides the
// starting point, and a bad guess costs a trip to the settings screen rather
// than an unplayable first launch.
//
// The classification is a pure function of a descriptor struct, so the table
// is exercised on the host against real device strings in
// ports/android/tests/DeviceProfileTests.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace kisak::perf {

enum class QualityTier : uint8_t
{
    // 64-bit but weak: early Snapdragon 4xx/6xx, Mali-T8xx, 2-3 GB RAM.
    Minimum,
    Low,
    Medium,
    High,
    // Flagship: Adreno 7xx, Immortalis, Xclipse, Apple-class throughput.
    Ultra,
};

const char *QualityTierName(QualityTier tier);

struct DeviceDescriptor
{
    // GL_RENDERER / VkPhysicalDeviceProperties::deviceName, verbatim.
    std::string gpuRenderer;
    std::string gpuVendor;
    // ro.soc.model or ro.board.platform.
    std::string soc;
    std::string deviceModel;
    uint32_t totalRamMB = 0;
    int totalCores = 0;
    int bigCores = 0;
    // Highest big-core clock in kHz.
    int64_t maxFrequencyKhz = 0;
    // VK_API_VERSION as reported, or 0 when Vulkan is unavailable.
    uint32_t vulkanApiVersion = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    double displayHz = 60.0;
    bool supportsAstc = false;
    bool supportsEtc2 = true;
    // Driver reported an immediate-mode (desktop-like) architecture rather
    // than a tiler. Affects whether a depth prepass is worth doing.
    bool immediateModeRenderer = false;
};

// Resolved engine configuration. Field names follow the dvars they set so the
// mapping in device_profile.cpp stays obvious.
struct QualityProfile
{
    QualityTier tier = QualityTier::Medium;
    // Multiplier on the native display resolution for the 3D render target.
    float renderScale = 1.0f;
    // Hard floor the dynamic scaler may not go below.
    float minRenderScale = 0.6f;
    bool dynamicResolution = true;
    int targetFps = 60;

    // r_picmip family: 0 is full resolution, higher drops mip levels.
    int picmip = 0;
    int picmipBump = 0;
    int picmipSpec = 0;
    // r_texFilterAnisoMax
    int anisotropy = 4;
    // r_lodBiasRigid / r_lodBiasSkinned, in world units.
    float lodBias = 0.0f;
    // r_dlightLimit
    int dynamicLightLimit = 4;
    // r_shadows: 0 off, 1 blob, 2 projected.
    int shadows = 2;
    // r_specular / r_normalMap toggles.
    bool specular = true;
    bool normalMaps = true;
    // fx_enable / effect density multiplier.
    float effectDensity = 1.0f;
    // r_distortion, the heat-haze and glass pass.
    bool distortion = true;
    // Bloom/filmic post. The heaviest full-screen pass in the renderer.
    bool postProcess = true;
    // 1 = none. 2 and 4 are MSAA sample counts; only offered where the tiler
    // can do it in-tile and it is therefore nearly free.
    int msaaSamples = 1;
    // snd_maxChannels equivalent.
    int soundChannels = 32;
    // Depth prepass helps immediate-mode GPUs and hurts tilers, which already
    // resolve hidden surfaces in-tile.
    bool depthPrepass = false;

    // Extra console commands appended verbatim after the tier defaults,
    // for per-device workarounds.
    std::vector<std::string> extraCommands;

    // The whole profile as engine console commands, one per line, ready for
    // Cbuf_AddText. This is how the native side applies it.
    std::string ToConsoleCommands() const;
};

// Classifies the GPU alone. Exposed because the launcher shows it before the
// engine starts.
QualityTier ClassifyGpu(const std::string &renderer, const std::string &vendor);

// Full classification, taking memory and CPU into account: a flagship GPU in
// a 3 GB phone still cannot hold the working set.
QualityTier ClassifyDevice(const DeviceDescriptor &device);

QualityProfile ProfileForTier(QualityTier tier, const DeviceDescriptor &device);

inline QualityProfile ProfileForDevice(const DeviceDescriptor &device)
{
    return ProfileForTier(ClassifyDevice(device), device);
}

} // namespace kisak::perf
