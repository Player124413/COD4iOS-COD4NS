#pragma once

// One object the engine talks to per frame.
//
// The pacer, the resolution scaler and the thermal governor each solve one
// problem, and they interact: the governor caps what the scaler may ask for,
// the scaler changes the cost the pacer predicts, and the pacer's cadence
// defines the budget both of them measure against. Wiring that up at every
// call site would guarantee the three drift apart, so the director owns the
// order of operations and exposes a single frame-shaped API:
//
//     director.BeginFrame(vsyncNs);
//     ... simulate and submit ...
//     director.EndCpuWork(nowNs);
//     ... present ...
//     director.EndFrame(nowNs);          // returns this frame's decisions
//     sleep(director.SleepBudgetNs(now));
//
// ports/android/engine/android_sys.cpp drives it from the engine's frame loop
// and ports/android/app from the native activity.

#include "cpu_topology.h"
#include "device_profile.h"
#include "frame_pacer.h"
#include "resolution_scaler.h"
#include "thermal_governor.h"

#include <cstdint>
#include <string>

namespace kisak::perf {

class PerfDirector
{
public:
    struct FrameResult
    {
        float renderScale = 1.0f;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        bool resolutionChanged = false;
        bool thermalThrottling = false;
        int targetFps = 60;
        Bottleneck bottleneck = Bottleneck::Unknown;
    };

    PerfDirector();

    // Called once, after the GPU backend reports what it found.
    void Initialise(const DeviceDescriptor &device, const QualityProfile &profile);

    // Swapchain size changed (rotation, fold, mode change).
    void SetNativeResolution(uint32_t width, uint32_t height);
    void SetDisplayRefreshRate(double hz);

    // Player overrides from the launcher.
    void SetDynamicResolutionEnabled(bool enabled);
    void SetManualRenderScale(float scale);
    void SetTargetFps(int fps);

    // Platform thermal signals, sampled by the app layer.
    void OnThermalStatus(ThermalStatus status);
    void OnThermalHeadroom(float headroom);

    int64_t BeginFrame(int64_t vsyncNs);
    void EndCpuWork(int64_t nowNs);
    void ReportGpuTime(int64_t gpuNs);
    FrameResult EndFrame(int64_t nowNs);

    int64_t SleepBudgetNs(int64_t nowNs) const { return m_pacer.SleepBudgetNs(nowNs); }
    bool ShouldSpin(int64_t remainingNs) const { return m_pacer.ShouldSpin(remainingNs); }

    const FramePacer &pacer() const { return m_pacer; }
    const ResolutionScaler &scaler() const { return m_scaler; }
    const ThermalGovernor &thermal() const { return m_thermal; }
    const CpuTopology &topology() const { return m_topology; }
    const QualityProfile &profile() const { return m_profile; }

    // One line for the on-screen overlay and the log, e.g.
    //   "59.8 fps  cpu 9.1  gpu 13.4  scale 0.90 (1728x810)  GPU-bound"
    std::string StatusLine() const;

    // Places the calling thread according to its role. Called once per thread
    // at creation; see ports/android/engine/android_sys.cpp.
    bool PlaceThread(ThreadRole role);

private:
    FramePacer m_pacer;
    ResolutionScaler m_scaler;
    ThermalGovernor m_thermal;
    CpuTopology m_topology;
    QualityProfile m_profile;
    DeviceDescriptor m_device;

    FrameSample m_lastSample{};
    FrameResult m_lastResult{};
    int64_t m_frameStartNs = 0;
    int64_t m_cpuEndNs = 0;
    int64_t m_gpuNs = 0;
    bool m_initialised = false;
};

// Process-wide instance. The engine is a singleton anyway (one hunk, one
// renderer), and threading a director pointer through decompiled call sites
// that take no context argument is not practical.
PerfDirector &Director();

} // namespace kisak::perf
