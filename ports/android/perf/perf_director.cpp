#include "perf_director.h"

#include <algorithm>
#include <cstdio>

namespace kisak::perf {

PerfDirector::PerfDirector() = default;

void PerfDirector::Initialise(const DeviceDescriptor &device, const QualityProfile &profile)
{
    m_device = device;
    m_profile = profile;
    m_initialised = true;

    m_topology = DiscoverTopology(device.totalCores > 0 ? device.totalCores : 8);

    FramePacer::Config pacing;
    pacing.targetFps = profile.targetFps;
    pacing.displayHz = device.displayHz;
    m_pacer.Configure(pacing);

    ResolutionScaler::Config scaling;
    scaling.minScale = profile.minRenderScale;
    scaling.maxScale = 1.0f;
    m_scaler.Configure(scaling);
    m_scaler.SetEnabled(profile.dynamicResolution);
    m_scaler.SetScaleCeiling(profile.renderScale);
    if (device.displayWidth && device.displayHeight)
        m_scaler.SetNativeResolution(device.displayWidth, device.displayHeight);

    m_thermal.Reset();
}

void PerfDirector::SetNativeResolution(uint32_t width, uint32_t height)
{
    m_device.displayWidth = width;
    m_device.displayHeight = height;
    m_scaler.SetNativeResolution(width, height);
}

void PerfDirector::SetDisplayRefreshRate(double hz)
{
    m_device.displayHz = hz;
    m_pacer.SetDisplayRefreshRate(hz);
}

void PerfDirector::SetDynamicResolutionEnabled(bool enabled)
{
    m_profile.dynamicResolution = enabled;
    m_scaler.SetEnabled(enabled);
}

void PerfDirector::SetManualRenderScale(float scale)
{
    m_profile.renderScale = std::clamp(scale, 0.3f, 1.0f);
    m_scaler.SetScaleCeiling(m_profile.renderScale);
}

void PerfDirector::SetTargetFps(int fps)
{
    m_profile.targetFps = fps;
    m_pacer.SetTargetFps(fps);
}

void PerfDirector::OnThermalStatus(ThermalStatus status)
{
    m_thermal.OnThermalStatus(status);
}

void PerfDirector::OnThermalHeadroom(float headroom)
{
    m_thermal.OnThermalHeadroom(headroom);
}

int64_t PerfDirector::BeginFrame(int64_t vsyncNs)
{
    m_frameStartNs = vsyncNs;
    m_cpuEndNs = vsyncNs;
    m_gpuNs = 0;
    return m_pacer.BeginFrame(vsyncNs);
}

void PerfDirector::EndCpuWork(int64_t nowNs)
{
    m_cpuEndNs = nowNs;
    m_pacer.EndCpuWork(nowNs);
}

void PerfDirector::ReportGpuTime(int64_t gpuNs)
{
    m_gpuNs = gpuNs;
    m_pacer.ReportGpuTime(gpuNs);
}

PerfDirector::FrameResult PerfDirector::EndFrame(int64_t nowNs)
{
    m_pacer.ReportRenderedPixels(m_scaler.width() * m_scaler.height());
    m_pacer.EndFrame(nowNs);

    m_lastSample = FrameSample{};
    m_lastSample.cpuNs = std::max<int64_t>(0, m_cpuEndNs - m_frameStartNs);
    m_lastSample.gpuNs = m_gpuNs;
    m_lastSample.frameNs = std::max<int64_t>(0, nowNs - m_frameStartNs);
    m_lastSample.renderedPixels = m_scaler.width() * m_scaler.height();
    m_lastSample.missedDeadline = m_lastSample.frameNs > m_pacer.frameBudgetNs();

    // Thermal first: its ceiling is an input to the scaler, not a parallel
    // opinion. Running it the other way round lets the scaler raise into a
    // range the governor has already ruled out, for one frame, every frame.
    const ThermalGovernor::State thermal = m_thermal.Update();
    const float ceiling = std::min(m_profile.renderScale, thermal.scaleCeiling);
    m_scaler.SetScaleCeiling(ceiling);
    if (thermal.targetFps != m_pacer.config().targetFps)
        m_pacer.SetTargetFps(thermal.targetFps);

    const FramePacer::Stats stats = m_pacer.stats();
    const ResolutionScaler::Decision decision =
        m_scaler.Update(m_lastSample, stats.bottleneck, m_pacer.frameBudgetNs());

    FrameResult result;
    result.renderScale = decision.scale;
    result.renderWidth = decision.width;
    result.renderHeight = decision.height;
    result.resolutionChanged = decision.changed;
    result.thermalThrottling = thermal.throttling;
    result.targetFps = thermal.targetFps;
    result.bottleneck = stats.bottleneck;
    m_lastResult = result;
    return result;
}

std::string PerfDirector::StatusLine() const
{
    const FramePacer::Stats stats = m_pacer.stats();
    const char *bound = "?";
    switch (stats.bottleneck)
    {
    case Bottleneck::Cpu: bound = "CPU-bound"; break;
    case Bottleneck::Gpu: bound = "GPU-bound"; break;
    case Bottleneck::Balanced: bound = "balanced"; break;
    case Bottleneck::Unknown: bound = "measuring"; break;
    }
    const double fps = stats.averageFrameMs > 0.0 ? 1000.0 / stats.averageFrameMs : 0.0;
    char line[256];
    std::snprintf(line, sizeof(line), "%.1f fps  cpu %.1f  gpu %.1f  p95 %.1f  scale %.2f (%ux%u)  %s%s",
                  fps, stats.averageCpuMs, stats.averageGpuMs, stats.p95FrameMs,
                  static_cast<double>(m_scaler.scale()), m_scaler.width(), m_scaler.height(), bound,
                  m_lastResult.thermalThrottling ? "  [thermal]" : "");
    return std::string(line);
}

bool PerfDirector::PlaceThread(ThreadRole role)
{
    const bool pinned = ApplyAffinity(m_topology, role);
    const bool prioritised = RequestHighPriority(role);
    return pinned && prioritised;
}

PerfDirector &Director()
{
    static PerfDirector instance;
    return instance;
}

} // namespace kisak::perf
