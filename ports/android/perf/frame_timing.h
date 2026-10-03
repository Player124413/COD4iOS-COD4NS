#pragma once

// Shared frame-timing vocabulary for the Android port's performance subsystem.
//
// Every tuner in ports/android/perf consumes the same measurements, so they are
// defined once here instead of being re-derived per component. All times are
// nanoseconds on CLOCK_MONOTONIC, which is what AChoreographer, the Vulkan
// present-timing extension and clock_gettime() all report.
//
// Nothing in this header touches an Android API, so the controllers that build
// on it run and are tested on the host.

#include <cstdint>

namespace kisak::perf {

inline constexpr int64_t kNsPerMs = 1000000;
inline constexpr int64_t kNsPerSecond = 1000000000;

// One completed frame, measured at the three points that matter when deciding
// whether 60 fps is being held and, if not, which side is responsible.
struct FrameSample
{
    // Wall time from the start of simulation to the end of command submission.
    int64_t cpuNs = 0;
    // GPU execution time, from timestamp queries around the frame's work.
    // 0 when the device cannot report it; the tuners then fall back to wall time.
    int64_t gpuNs = 0;
    // Start-of-frame to start-of-next-frame. This is the number the player feels.
    int64_t frameNs = 0;
    // Pixels actually rendered this frame, before upscaling to the swapchain.
    // Lets the resolution controller reason in cost-per-pixel rather than
    // guessing how much a scale change bought.
    uint32_t renderedPixels = 0;
    // True when the frame reached the compositor after its vsync deadline.
    bool missedDeadline = false;
};

// Which side of the pipeline is setting the frame time. The resolution scaler
// only helps when the GPU is the limit; lowering resolution on a CPU-bound
// frame costs image quality and buys nothing.
enum class Bottleneck : uint8_t
{
    Unknown,
    Cpu,
    Gpu,
    Balanced, // within measurement noise of each other
};

// Android's PowerManager.THERMAL_STATUS_* ladder. Mirrored here so the
// governor is testable without an Android runtime.
enum class ThermalStatus : uint8_t
{
    None = 0,
    Light = 1,
    Moderate = 2,
    Severe = 3,
    Critical = 4,
    Emergency = 5,
    Shutdown = 6,
};

inline constexpr int64_t FpsToPeriodNs(int fps)
{
    return fps > 0 ? kNsPerSecond / fps : 0;
}

inline constexpr double NsToMs(int64_t ns)
{
    return static_cast<double>(ns) / 1.0e6;
}

} // namespace kisak::perf
