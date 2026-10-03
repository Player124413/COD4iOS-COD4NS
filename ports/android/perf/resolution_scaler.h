#pragma once

// Dynamic resolution for the Android port.
//
// Phone GPUs vary by more than an order of magnitude, and the same phone
// varies by 40% between its first minute and its tenth. A fixed render
// resolution therefore has to be chosen for the worst case, which wastes the
// best case. This controller instead holds the frame time at the target and
// lets resolution be the thing that moves.
//
// Behaviour that matters in practice:
//
//   * It only acts when the GPU is the bottleneck. Dropping resolution on a
//     CPU-bound frame costs sharpness and buys nothing.
//   * It drops fast and recovers slowly. A dropped frame is visible now; a
//     slightly soft image for another second is not.
//   * Scale steps are quantised and the result is rounded to a multiple of 8
//     pixels, so the swapchain blit stays cheap and tile bins stay aligned.
//   * A dwell time between changes stops it oscillating between two scales,
//     which is far more noticeable than either scale on its own.
//
// Pure arithmetic, no Android API: ports/android/tests/ResolutionScalerTests.cpp
// drives it on the host.

#include "frame_timing.h"

#include <cstdint>

namespace kisak::perf {

class ResolutionScaler
{
public:
    struct Config
    {
        // Lower bound on the linear scale. 0.6 is roughly 1/3 of the pixels;
        // below that the upscale is obvious even on a phone screen.
        float minScale = 0.60f;
        float maxScale = 1.00f;
        // Granularity of a change. Smaller steps are smoother but take longer
        // to escape a deep drop.
        float step = 0.05f;
        // Fraction of the frame budget above which the scaler reduces. 0.92
        // leaves headroom so it reacts before frames are actually missed.
        float reduceThreshold = 0.92f;
        // Fraction below which it is safe to give resolution back.
        float raiseThreshold = 0.75f;
        // Frames to wait after a change before considering another, so the
        // measurement reflects the new scale.
        int dwellFrames = 10;
        // Consecutive qualifying frames before raising. Recovery should need
        // more evidence than a drop does.
        int raiseConfirmFrames = 45;
        // Render target dimensions are rounded down to a multiple of this.
        uint32_t alignment = 8;
    };

    struct Decision
    {
        float scale = 1.0f;
        bool changed = false;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    ResolutionScaler();
    explicit ResolutionScaler(const Config &config);

    void Configure(const Config &config);
    const Config &config() const { return m_config; }

    // Native swapchain size the scale is applied to.
    void SetNativeResolution(uint32_t width, uint32_t height);

    // Hard ceiling from the quality profile or the thermal governor. The
    // scaler never exceeds it even when it has headroom.
    void SetScaleCeiling(float ceiling);

    // Pin the scale (player chose a fixed resolution in the launcher).
    void SetEnabled(bool enabled);
    bool enabled() const { return m_enabled; }

    // One frame of evidence. frameBudgetNs comes from the pacer, so the
    // scaler automatically targets whatever cadence is in force.
    Decision Update(const FrameSample &sample, Bottleneck bottleneck, int64_t frameBudgetNs);

    float scale() const { return m_scale; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    void Reset();

private:
    void Apply(float scale);

    Config m_config;
    bool m_enabled = true;
    float m_scale = 1.0f;
    float m_ceiling = 1.0f;
    uint32_t m_nativeWidth = 0;
    uint32_t m_nativeHeight = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int m_dwell = 0;
    int m_raiseStreak = 0;
};

} // namespace kisak::perf
