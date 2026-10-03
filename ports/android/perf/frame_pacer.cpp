#include "frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace kisak::perf {

namespace {

// Panel rates that phones actually ship. A measured refresh rate is never
// exactly one of these (120 Hz reports as 119.9 or 120.00002), so the cadence
// search snaps to the nearest entry before looking for divisors.
constexpr double kKnownRates[] = { 48.0, 50.0, 60.0, 72.0, 90.0, 96.0, 100.0, 120.0, 144.0, 165.0, 240.0 };

double SnapRefreshRate(double hz)
{
    if (!(hz > 1.0) || !std::isfinite(hz))
        return 60.0;
    double best = hz;
    double bestError = 1.0e9;
    for (const double candidate : kKnownRates)
    {
        const double error = std::fabs(candidate - hz);
        // 1.5 Hz covers reporting slop without merging 90 and 96.
        if (error < bestError && error <= 1.5)
        {
            best = candidate;
            bestError = error;
        }
    }
    return best;
}

double Percentile(std::vector<double> &values, double fraction)
{
    if (values.empty())
        return 0.0;
    const std::size_t index =
        std::min(values.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

} // namespace

FramePacer::FramePacer()
{
    Configure(Config{});
}

FramePacer::FramePacer(const Config &config)
{
    Configure(config);
}

void FramePacer::Configure(const Config &config)
{
    m_config = config;
    m_config.targetFps = std::clamp(m_config.targetFps, 15, 240);
    m_config.safetyMarginFraction = std::clamp(m_config.safetyMarginFraction, 0.0, 0.5);
    m_config.historyFrames = std::clamp<std::size_t>(m_config.historyFrames, 8, kCapacity);
    m_cadence = ChooseCadence(m_config.targetFps, m_config.displayHz, m_config.allowFrameDivisors);
    m_frameBudgetNs = m_cadence.presentedFps > 0.0
                          ? static_cast<int64_t>(static_cast<double>(kNsPerSecond) / m_cadence.presentedFps)
                          : FpsToPeriodNs(m_config.targetFps);
    Recompute();
}

void FramePacer::SetDisplayRefreshRate(double hz)
{
    if (std::fabs(hz - m_config.displayHz) < 0.01)
        return;
    Config next = m_config;
    next.displayHz = hz;
    Configure(next);
    // The cost history was gathered against a different cadence; keeping it
    // would make the first frames after a mode switch mispredict badly.
    Reset();
}

void FramePacer::SetTargetFps(int fps)
{
    if (fps == m_config.targetFps)
        return;
    Config next = m_config;
    next.targetFps = fps;
    Configure(next);
}

FramePacer::Cadence FramePacer::ChooseCadence(int targetFps, double displayHz, bool allowDivisors)
{
    Cadence result;
    const double panel = SnapRefreshRate(displayHz);
    const double target = static_cast<double>(std::clamp(targetFps, 15, 240));

    result.swapInterval = 1;
    result.presentedFps = panel;
    result.requestedDisplayHz = panel;
    result.needsModeChange = false;

    if (!allowDivisors)
    {
        result.presentedFps = std::min(panel, target);
        return result;
    }

    // Look for an integer divisor of the panel rate that lands on the target.
    // 120 Hz / 2 = 60 exactly; 144 Hz / 2 = 72, / 3 = 48, neither of which is
    // 60, so that panel needs a mode change to hold a true 60.
    int bestInterval = 1;
    double bestError = std::fabs(panel - target);
    for (int interval = 1; interval <= 4; ++interval)
    {
        const double fps = panel / interval;
        if (fps < target - 0.5)
            break; // dividing further only moves away from the target
        const double error = std::fabs(fps - target);
        if (error < bestError - 1e-6)
        {
            bestError = error;
            bestInterval = interval;
        }
    }

    result.swapInterval = bestInterval;
    result.presentedFps = panel / bestInterval;

    // Within half a frame of the target is as good as exact; the difference is
    // below perception and a mode switch costs a visible black frame.
    if (bestError > 0.5)
    {
        result.needsModeChange = true;
        result.requestedDisplayHz = target;
    }
    return result;
}

int64_t FramePacer::BeginFrame(int64_t vsyncNs)
{
    m_current = FrameSample{};
    m_frameStartNs = vsyncNs;
    m_cpuEndNs = vsyncNs;
    m_deadlineNs = vsyncNs + m_frameBudgetNs;
    m_inFrame = true;
    return m_deadlineNs;
}

void FramePacer::EndCpuWork(int64_t nowNs)
{
    if (!m_inFrame)
        return;
    m_cpuEndNs = nowNs;
    m_current.cpuNs = std::max<int64_t>(0, nowNs - m_frameStartNs);
}

void FramePacer::ReportGpuTime(int64_t gpuNs)
{
    if (gpuNs > 0)
        m_current.gpuNs = gpuNs;
}

void FramePacer::ReportRenderedPixels(uint32_t pixels)
{
    m_current.renderedPixels = pixels;
}

void FramePacer::EndFrame(int64_t nowNs)
{
    if (!m_inFrame)
        return;
    m_inFrame = false;
    m_current.frameNs = std::max<int64_t>(0, nowNs - m_frameStartNs);
    if (m_current.cpuNs == 0)
        m_current.cpuNs = m_current.frameNs;
    // A frame that ran past its deadline displaced a refresh, whatever the
    // compositor reports; that is the condition the tuners react to.
    m_current.missedDeadline = nowNs > m_deadlineNs;
    Push(m_current);
    Recompute();
}

void FramePacer::Push(const FrameSample &sample)
{
    m_history[m_next] = sample;
    m_next = (m_next + 1) % kCapacity;
    if (m_count < kCapacity)
        ++m_count;
}

void FramePacer::Recompute()
{
    const std::size_t window = std::min(m_count, m_config.historyFrames);
    Stats stats;
    stats.samples = window;
    stats.presentedFps = m_cadence.presentedFps;
    if (window == 0)
    {
        m_stats = stats;
        // With no history, reserve the whole budget: better to under-sleep on
        // the first frames than to miss them.
        m_predictedCostNs = m_frameBudgetNs;
        return;
    }

    std::vector<double> frameMs;
    frameMs.reserve(window);
    double cpuSum = 0.0;
    double gpuSum = 0.0;
    double frameSum = 0.0;
    std::size_t gpuSamples = 0;
    std::size_t misses = 0;

    for (std::size_t i = 0; i < window; ++i)
    {
        // Walk backwards from the newest entry so the window is the most
        // recent `window` frames regardless of ring position.
        const std::size_t index = (m_next + kCapacity - 1 - i) % kCapacity;
        const FrameSample &sample = m_history[index];
        frameMs.push_back(NsToMs(sample.frameNs));
        frameSum += NsToMs(sample.frameNs);
        cpuSum += NsToMs(sample.cpuNs);
        if (sample.gpuNs > 0)
        {
            gpuSum += NsToMs(sample.gpuNs);
            ++gpuSamples;
        }
        if (sample.missedDeadline)
            ++misses;
    }

    const double count = static_cast<double>(window);
    stats.averageFrameMs = frameSum / count;
    stats.averageCpuMs = cpuSum / count;
    stats.averageGpuMs = gpuSamples ? gpuSum / static_cast<double>(gpuSamples) : 0.0;
    stats.missRatio = static_cast<double>(misses) / count;
    stats.p95FrameMs = Percentile(frameMs, 0.95);

    if (gpuSamples >= window / 2 && gpuSamples > 0 && stats.averageGpuMs > 0.0)
    {
        // Ten percent is comfortably outside query and scheduling noise, and
        // small enough to classify a frame that is genuinely one-sided.
        const double ratio = stats.averageGpuMs / std::max(stats.averageCpuMs, 0.0001);
        if (ratio > 1.10)
            stats.bottleneck = Bottleneck::Gpu;
        else if (ratio < 0.90)
            stats.bottleneck = Bottleneck::Cpu;
        else
            stats.bottleneck = Bottleneck::Balanced;
    }
    else if (stats.averageCpuMs > 0.0)
    {
        // Without GPU timestamps, treat time spent outside CPU work (waiting
        // on the driver) as GPU time.
        const double nonCpu = stats.averageFrameMs - stats.averageCpuMs;
        stats.bottleneck = nonCpu > stats.averageCpuMs * 0.25 ? Bottleneck::Gpu : Bottleneck::Cpu;
    }

    m_stats = stats;

    // Reserve the 95th percentile, not the mean: the mean leaves one frame in
    // twenty landing late, which reads as stutter even though the average is
    // fine. Never reserve more than the budget, or the pacer would stop
    // sleeping entirely and spin a core for nothing.
    const int64_t p95Ns = static_cast<int64_t>(stats.p95FrameMs * 1.0e6);
    m_predictedCostNs = std::clamp<int64_t>(p95Ns, 0, m_frameBudgetNs);
}

int64_t FramePacer::PredictedFrameCostNs() const
{
    return m_predictedCostNs;
}

int64_t FramePacer::SleepBudgetNs(int64_t nowNs) const
{
    const int64_t margin = static_cast<int64_t>(static_cast<double>(m_frameBudgetNs) * m_config.safetyMarginFraction);
    // Start late enough that the predicted frame still lands before the
    // deadline, with the margin held back for a frame that runs long.
    const int64_t startBy = m_deadlineNs - m_predictedCostNs - margin;
    return std::max<int64_t>(0, startBy - nowNs);
}

bool FramePacer::ShouldSpin(int64_t remainingNs) const
{
    return remainingNs > 0 && remainingNs <= m_config.spinThresholdNs;
}

FramePacer::Stats FramePacer::stats() const
{
    return m_stats;
}

void FramePacer::Reset()
{
    m_count = 0;
    m_next = 0;
    m_stats = Stats{};
    m_stats.presentedFps = m_cadence.presentedFps;
    m_predictedCostNs = m_frameBudgetNs;
    m_inFrame = false;
}

} // namespace kisak::perf
