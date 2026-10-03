#pragma once

// Frame pacing for a locked 60 fps on Android.
//
// A game that simply renders as fast as it can does not feel like 60 fps on a
// phone. Three Android-specific effects get in the way:
//
//   1. Panels are rarely 60 Hz any more. 90, 120 and 144 Hz are common, and a
//      free-running 60 fps renderer on a 120 Hz panel beats against the vsync
//      cadence, producing a 50/50 mix of 8.3 ms and 16.7 ms frames.
//   2. SurfaceFlinger buffers. Submitting earlier than necessary fills the
//      queue and adds a frame of latency without adding smoothness.
//   3. Thermals. Running the GPU flat out to produce frames the compositor
//      throws away heats the phone until the clocks drop and the frame rate
//      collapses, which is the usual reason a mobile port starts at 60 and
//      settles at 40.
//
// This pacer fixes a presentation cadence (an integer number of display
// refreshes per rendered frame), predicts how long the next frame will take
// from recent history, and reports the instant at which work should start so
// the frame lands just before its deadline instead of as early as possible.
// The caller sleeps for the reported budget, which gives the scheduler room to
// drop clocks and keeps the device cool enough to sustain the cadence.
//
// The class is pure arithmetic over injected timestamps: no Android API, no
// clock reads of its own. ports/android/app binds it to AChoreographer, and
// ports/android/tests/FramePacerTests.cpp drives it on the host.

#include "frame_timing.h"

#include <cstddef>

namespace kisak::perf {

class FramePacer
{
public:
    struct Config
    {
        // Frames per second the player should see.
        int targetFps = 60;
        // Panel refresh rate. Updated at runtime from the display mode.
        double displayHz = 60.0;
        // Allow presenting once every N refreshes to reach the target. Without
        // this a 120 Hz panel either runs at 120 (hot) or free-runs (uneven).
        bool allowFrameDivisors = true;
        // How much of the frame budget to hold back as insurance against a
        // frame that runs longer than predicted. 12% of 16.67 ms is ~2 ms.
        double safetyMarginFraction = 0.12;
        // Samples kept for the cost estimate. One second at 60 fps reacts
        // quickly without chasing a single slow frame.
        std::size_t historyFrames = 60;
        // Spin rather than sleep once the remaining wait is this short, since
        // a sleep that short routinely overshoots on a loaded Android kernel.
        int64_t spinThresholdNs = 1200000; // 1.2 ms
    };

    struct Cadence
    {
        // Display refreshes between presents. 1 on a 60 Hz panel, 2 on 120 Hz.
        int swapInterval = 1;
        // The frame rate that cadence actually produces.
        double presentedFps = 60.0;
        // Refresh rate to ask the platform for via Surface.setFrameRate().
        // On a 90 Hz panel no divisor yields 60, so the right move is to
        // request a 60 Hz mode rather than accept 90 or 45.
        double requestedDisplayHz = 60.0;
        // True when no integer divisor of the current panel rate hits the
        // target. The app layer uses this to decide whether requesting a mode
        // change is worth the switch.
        bool needsModeChange = false;
    };

    struct Stats
    {
        double averageFrameMs = 0.0;
        double p95FrameMs = 0.0;
        double averageCpuMs = 0.0;
        double averageGpuMs = 0.0;
        double presentedFps = 0.0;
        // Fraction of the recent window that missed its deadline, 0..1.
        double missRatio = 0.0;
        Bottleneck bottleneck = Bottleneck::Unknown;
        std::size_t samples = 0;
    };

    FramePacer();
    explicit FramePacer(const Config &config);

    void Configure(const Config &config);
    const Config &config() const { return m_config; }

    // Panel rate changed (mode switch, external display, user setting).
    void SetDisplayRefreshRate(double hz);
    void SetTargetFps(int fps);

    const Cadence &cadence() const { return m_cadence; }
    int64_t frameBudgetNs() const { return m_frameBudgetNs; }

    // Called with the timestamp the compositor reports for the current vsync
    // (AChoreographer's frameTimeNanos). Establishes the deadline this frame
    // is working towards. Returns that deadline.
    int64_t BeginFrame(int64_t vsyncNs);

    // Called after simulation plus command submission, before present.
    void EndCpuWork(int64_t nowNs);

    // GPU time for the frame that just completed, from timestamp queries.
    // Optional: the pacer degrades to wall-clock reasoning without it.
    void ReportGpuTime(int64_t gpuNs);

    // Pixels rendered this frame, so the resolution controller can price them.
    void ReportRenderedPixels(uint32_t pixels);

    // Called once the frame has been presented. Closes the sample.
    void EndFrame(int64_t nowNs);

    // How long to wait before starting the next frame so that it finishes just
    // inside its deadline rather than as early as possible. Never negative.
    int64_t SleepBudgetNs(int64_t nowNs) const;

    // True when the remaining wait is short enough that spinning beats
    // sleeping. The app layer uses this to choose between nanosleep and a
    // yield loop.
    bool ShouldSpin(int64_t remainingNs) const;

    // Predicted cost of the next frame: a high-percentile estimate, not the
    // mean, because the mean under-reserves and a late frame costs a whole
    // refresh period.
    int64_t PredictedFrameCostNs() const;

    Stats stats() const;
    void Reset();

    // Picks the cadence for a target and panel rate. Exposed for tests and for
    // the launcher, which previews the result before the engine starts.
    static Cadence ChooseCadence(int targetFps, double displayHz, bool allowDivisors);

private:
    void Push(const FrameSample &sample);
    void Recompute();

    Config m_config;
    Cadence m_cadence;
    int64_t m_frameBudgetNs = FpsToPeriodNs(60);

    // Ring buffer of recent frames. A fixed capacity keeps the pacer
    // allocation-free once configured, which matters because it runs inside
    // the frame loop.
    static constexpr std::size_t kCapacity = 240;
    FrameSample m_history[kCapacity]{};
    std::size_t m_count = 0;
    std::size_t m_next = 0;

    FrameSample m_current{};
    int64_t m_frameStartNs = 0;
    int64_t m_cpuEndNs = 0;
    int64_t m_deadlineNs = 0;
    bool m_inFrame = false;

    Stats m_stats{};
    int64_t m_predictedCostNs = 0;
};

} // namespace kisak::perf
