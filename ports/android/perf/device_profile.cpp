#include "device_profile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kisak::perf {

namespace {

std::string Lower(const std::string &text)
{
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool Contains(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Reads the first run of digits after `prefix`. Adreno and Mali both encode
// their generation numerically, and the number is the only reliable signal:
// the rest of the string is vendor marketing that changes between drivers.
int NumberAfter(const std::string &text, const char *prefix)
{
    const std::size_t at = text.find(prefix);
    if (at == std::string::npos)
        return -1;
    std::size_t i = at + std::strlen(prefix);
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i])))
    {
        // Stop at a separator so "adreno (tm) 650" is read but an unrelated
        // number later in the string is not.
        if (text[i] != ' ' && text[i] != '(' && text[i] != ')' && text[i] != 't' && text[i] != 'm' && text[i] != '-')
            return -1;
        ++i;
    }
    if (i >= text.size())
        return -1;
    int value = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    {
        value = value * 10 + (text[i] - '0');
        ++i;
    }
    return value;
}

void AppendCommand(std::string &out, const char *name, int value)
{
    char line[128];
    std::snprintf(line, sizeof(line), "seta %s %d\n", name, value);
    out += line;
}

void AppendCommand(std::string &out, const char *name, float value)
{
    char line[128];
    std::snprintf(line, sizeof(line), "seta %s %.3f\n", name, static_cast<double>(value));
    out += line;
}

} // namespace

const char *QualityTierName(QualityTier tier)
{
    switch (tier)
    {
    case QualityTier::Minimum: return "Minimum";
    case QualityTier::Low: return "Low";
    case QualityTier::Medium: return "Medium";
    case QualityTier::High: return "High";
    case QualityTier::Ultra: return "Ultra";
    }
    return "Medium";
}

QualityTier ClassifyGpu(const std::string &rendererText, const std::string &vendorText)
{
    const std::string renderer = Lower(rendererText);
    const std::string vendor = Lower(vendorText);

    // --- Qualcomm Adreno -----------------------------------------------
    // The series digit tracks architecture; the last two digits track the
    // bin within it. 6xx spans five years of parts, so the bin matters.
    if (Contains(renderer, "adreno"))
    {
        const int model = NumberAfter(renderer, "adreno");
        if (model >= 700) return QualityTier::Ultra;          // 730/740/750 and up
        if (model >= 660) return QualityTier::Ultra;          // 660/680/690 flagships
        if (model >= 640) return QualityTier::High;           // 640/642/650
        if (model >= 610) return QualityTier::Medium;         // 612/616/618/619/620
        if (model >= 540) return QualityTier::Medium;         // 540/630 era flagships
        if (model >= 500) return QualityTier::Low;            // 505/506/508/510/512
        if (model > 0) return QualityTier::Minimum;           // 4xx and older
        return QualityTier::Medium;
    }

    // --- Arm Mali ------------------------------------------------------
    if (Contains(renderer, "immortalis"))
        return QualityTier::Ultra;
    if (Contains(renderer, "mali"))
    {
        // Valhall and later ("Mali-G<number>"), then Bifrost, then Midgard.
        const int g = NumberAfter(renderer, "mali-g");
        if (g > 0)
        {
            if (g >= 710) return QualityTier::Ultra;
            if (g >= 68) return QualityTier::High;    // G68/G76/G77/G78
            if (g >= 57) return QualityTier::Medium;  // G57/G52 mid-range
            if (g >= 51) return QualityTier::Low;
            return QualityTier::Low;
        }
        const int t = NumberAfter(renderer, "mali-t");
        if (t > 0)
            return t >= 860 ? QualityTier::Low : QualityTier::Minimum;
        return QualityTier::Low;
    }

    // --- Samsung Xclipse (RDNA-derived) --------------------------------
    if (Contains(renderer, "xclipse"))
        return QualityTier::Ultra;

    // --- Imagination PowerVR -------------------------------------------
    if (Contains(renderer, "powervr"))
    {
        if (Contains(renderer, "bxm") || Contains(renderer, "bxe") || Contains(renderer, "cxt"))
            return QualityTier::Medium;
        if (Contains(renderer, "gm9") || Contains(renderer, "ge8")) return QualityTier::Low;
        return QualityTier::Minimum;
    }

    // --- Desktop-class parts in emulators, handhelds and Chromebooks ---
    if (Contains(vendor, "nvidia") || Contains(renderer, "geforce") || Contains(renderer, "tegra"))
        return QualityTier::Ultra;
    if (Contains(renderer, "radeon") || Contains(renderer, "amd"))
        return QualityTier::Ultra;
    if (Contains(renderer, "intel") || Contains(renderer, "llvmpipe") || Contains(renderer, "swiftshader"))
        // A software rasteriser will not hold 60 fps at any setting, but the
        // engine should still start so the port can be debugged on it.
        return Contains(renderer, "intel") ? QualityTier::Medium : QualityTier::Minimum;

    return QualityTier::Medium;
}

QualityTier ClassifyDevice(const DeviceDescriptor &device)
{
    QualityTier tier = ClassifyGpu(device.gpuRenderer, device.gpuVendor);

    auto demote = [&tier](QualityTier floorTier) { tier = std::min(tier, floorTier); };

    // Memory. COD4's hunk plus the fastfile working set wants about 700 MB
    // free; Android will kill the process before letting it have that on a
    // small device, so cap the texture budget by capping the tier.
    if (device.totalRamMB > 0)
    {
        if (device.totalRamMB < 2600) demote(QualityTier::Minimum);
        else if (device.totalRamMB < 3600) demote(QualityTier::Low);
        else if (device.totalRamMB < 5600) demote(QualityTier::High);
    }

    // CPU. The engine's main thread is largely serial, so a part with only
    // little cores cannot feed a fast GPU regardless of what the GPU is.
    if (device.totalCores > 0 && device.bigCores == 0)
        demote(QualityTier::Low);
    if (device.maxFrequencyKhz > 0 && device.maxFrequencyKhz < 1900000)
        demote(QualityTier::Low);

    // No Vulkan means the GLES fallback, which gives up the pipeline cache
    // and the explicit barriers; budget for it.
    if (device.vulkanApiVersion == 0)
        demote(QualityTier::Medium);

    return tier;
}

QualityProfile ProfileForTier(QualityTier tier, const DeviceDescriptor &device)
{
    QualityProfile p;
    p.tier = tier;
    p.targetFps = 60;

    switch (tier)
    {
    case QualityTier::Minimum:
        p.renderScale = 0.60f;
        p.minRenderScale = 0.45f;
        p.picmip = 3;
        p.picmipBump = 3;
        p.picmipSpec = 3;
        p.anisotropy = 1;
        p.lodBias = 48.0f;
        p.dynamicLightLimit = 0;
        p.shadows = 0;
        p.specular = false;
        p.normalMaps = false;
        p.effectDensity = 0.35f;
        p.distortion = false;
        p.postProcess = false;
        p.soundChannels = 16;
        break;
    case QualityTier::Low:
        p.renderScale = 0.72f;
        p.minRenderScale = 0.55f;
        p.picmip = 2;
        p.picmipBump = 2;
        p.picmipSpec = 2;
        p.anisotropy = 1;
        p.lodBias = 24.0f;
        p.dynamicLightLimit = 1;
        p.shadows = 1;
        p.specular = false;
        p.normalMaps = true;
        p.effectDensity = 0.55f;
        p.distortion = false;
        p.postProcess = false;
        p.soundChannels = 24;
        break;
    case QualityTier::Medium:
        p.renderScale = 0.85f;
        p.minRenderScale = 0.60f;
        p.picmip = 1;
        p.picmipBump = 1;
        p.picmipSpec = 1;
        p.anisotropy = 2;
        p.lodBias = 8.0f;
        p.dynamicLightLimit = 2;
        p.shadows = 1;
        p.specular = true;
        p.normalMaps = true;
        p.effectDensity = 0.8f;
        p.distortion = true;
        p.postProcess = false;
        p.soundChannels = 32;
        break;
    case QualityTier::High:
        p.renderScale = 1.0f;
        p.minRenderScale = 0.70f;
        p.picmip = 0;
        p.picmipBump = 0;
        p.picmipSpec = 1;
        p.anisotropy = 4;
        p.lodBias = 0.0f;
        p.dynamicLightLimit = 4;
        p.shadows = 2;
        p.specular = true;
        p.normalMaps = true;
        p.effectDensity = 1.0f;
        p.distortion = true;
        p.postProcess = true;
        p.soundChannels = 48;
        break;
    case QualityTier::Ultra:
        p.renderScale = 1.0f;
        p.minRenderScale = 0.80f;
        p.picmip = 0;
        p.picmipBump = 0;
        p.picmipSpec = 0;
        p.anisotropy = 8;
        p.lodBias = 0.0f;
        p.dynamicLightLimit = 4;
        p.shadows = 2;
        p.specular = true;
        p.normalMaps = true;
        p.effectDensity = 1.0f;
        p.distortion = true;
        p.postProcess = true;
        p.msaaSamples = 2;
        p.soundChannels = 64;
        break;
    }

    // A tiler resolves hidden surfaces inside the tile, so a depth prepass is
    // pure extra geometry cost there. Only the immediate-mode parts benefit.
    p.depthPrepass = device.immediateModeRenderer && tier >= QualityTier::High;

    // Very high resolution panels: a 1440p phone has 78% more pixels than a
    // 1080p one for no visible benefit at this art's texel density. Render at
    // 1080p-equivalent and let the compositor upscale.
    const uint32_t shortEdge = std::min(device.displayWidth, device.displayHeight);
    if (shortEdge > 1200 && shortEdge > 0)
    {
        const float cap = 1200.0f / static_cast<float>(shortEdge);
        p.renderScale = std::min(p.renderScale, cap);
        p.minRenderScale = std::min(p.minRenderScale, cap * 0.8f);
    }

    // MSAA on a tiler is cheap but not free, and it costs tile memory that a
    // mid-range part needs for the depth buffer. Only keep it when the panel
    // is small enough that aliasing is actually visible.
    if (p.msaaSamples > 1 && shortEdge > 1200)
        p.msaaSamples = 1;

    return p;
}

std::string QualityProfile::ToConsoleCommands() const
{
    std::string out;
    out.reserve(1024);
    out += "// generated by ports/android/perf/device_profile.cpp\n";

    AppendCommand(out, "r_picmip", picmip);
    AppendCommand(out, "r_picmip_bump", picmipBump);
    AppendCommand(out, "r_picmip_spec", picmipSpec);
    AppendCommand(out, "r_picmip_manual", 1);
    AppendCommand(out, "r_texFilterAnisoMax", anisotropy);
    AppendCommand(out, "r_texFilterAnisoMin", std::min(anisotropy, 2));
    AppendCommand(out, "r_lodBiasRigid", lodBias);
    AppendCommand(out, "r_lodBiasSkinned", lodBias);
    AppendCommand(out, "r_dlightLimit", dynamicLightLimit);
    AppendCommand(out, "r_shadows", shadows);
    AppendCommand(out, "r_specular", specular ? 1 : 0);
    AppendCommand(out, "r_normalMap", normalMaps ? 1 : 0);
    AppendCommand(out, "r_distortion", distortion ? 1 : 0);
    AppendCommand(out, "r_glow", postProcess ? 1 : 0);
    AppendCommand(out, "r_filmUseTweaks", postProcess ? 1 : 0);
    AppendCommand(out, "fx_marks", effectDensity > 0.5f ? 1 : 0);
    AppendCommand(out, "fx_drawClouds", effectDensity > 0.7f ? 1 : 0);
    AppendCommand(out, "snd_maxchannels", soundChannels);
    AppendCommand(out, "com_maxfps", targetFps);
    // The renderer reads these two from the Android layer rather than from a
    // D3D device description, so they are registered by the port itself.
    AppendCommand(out, "r_androidRenderScale", renderScale);
    AppendCommand(out, "r_androidDynamicResolution", dynamicResolution ? 1 : 0);
    AppendCommand(out, "r_androidMsaa", msaaSamples);
    AppendCommand(out, "r_androidDepthPrepass", depthPrepass ? 1 : 0);

    for (const std::string &command : extraCommands)
    {
        out += command;
        if (out.back() != '\n')
            out += '\n';
    }
    return out;
}

} // namespace kisak::perf
