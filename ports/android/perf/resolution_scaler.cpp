#include "resolution_scaler.h"

#include <algorithm>
#include <cmath>

namespace kisak::perf {

namespace {

uint32_t AlignDown(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1)
        return value;
    const uint32_t aligned = value - (value % alignment);
    return aligned ? aligned : alignment;
}

float Quantise(float scale, float step)
{
    if (step <= 0.0f)
        return scale;
    return std::round(scale / step) * step;
}

} // namespace

ResolutionScaler::ResolutionScaler()
{
    Configure(Config{});
}

ResolutionScaler::ResolutionScaler(const Config &config)
{
    Configure(config);
}

void ResolutionScaler::Configure(const Config &config)
{
    m_config = config;
    m_config.minScale = std::clamp(m_config.minScale, 0.25f, 1.0f);
    m_config.maxScale = std::clamp(m_config.maxScale, m_config.minScale, 1.0f);
    m_config.step = std::clamp(m_config.step, 0.01f, 0.25f);
    m_config.reduceThreshold = std::clamp(m_config.reduceThreshold, 0.5f, 1.2f);
    // Keep a real gap between the two thresholds. If they meet, every frame
    // qualifies as both too slow and too fast and the scale never settles.
    m_config.raiseThreshold = std::clamp(m_config.raiseThreshold, 0.3f, m_config.reduceThreshold - 0.05f);
    m_config.dwellFrames = std::max(1, m_config.dwellFrames);
    m_config.raiseConfirmFrames = std::max(1, m_config.raiseConfirmFrames);
    m_config.alignment = std::max<uint32_t>(1, m_config.alignment);
    m_ceiling = std::clamp(m_ceiling, m_config.minScale, m_config.maxScale);
    Apply(m_scale);
}

void ResolutionScaler::SetNativeResolution(uint32_t width, uint32_t height)
{
    m_nativeWidth = width;
    m_nativeHeight = height;
    Apply(m_scale);
}

void ResolutionScaler::SetScaleCeiling(float ceiling)
{
    m_ceiling = std::clamp(ceiling, m_config.minScale, m_config.maxScale);
    if (m_scale > m_ceiling)
        Apply(m_ceiling);
}

void ResolutionScaler::SetEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled)
    {
        // Returning to the ceiling is the least surprising behaviour: the
        // player turned the feature off, so give back what it took.
        Apply(m_ceiling);
        m_dwell = 0;
        m_raiseStreak = 0;
    }
}

void ResolutionScaler::Apply(float scale)
{
    m_scale = std::clamp(Quantise(scale, m_config.step), m_config.minScale, std::min(m_ceiling, m_config.maxScale));
    if (m_nativeWidth && m_nativeHeight)
    {
        m_width = AlignDown(static_cast<uint32_t>(std::lround(static_cast<float>(m_nativeWidth) * m_scale)),
                            m_config.alignment);
        m_height = AlignDown(static_cast<uint32_t>(std::lround(static_cast<float>(m_nativeHeight) * m_scale)),
                             m_config.alignment);
    }
}

ResolutionScaler::Decision ResolutionScaler::Update(const FrameSample &sample, Bottleneck bottleneck,
                                                    int64_t frameBudgetNs)
{
    Decision decision;
    decision.scale = m_scale;
    decision.width = m_width;
    decision.height = m_height;

    if (!m_enabled || frameBudgetNs <= 0)
        return decision;

    if (m_dwell > 0)
    {
        --m_dwell;
        return decision;
    }

    // Prefer measured GPU time. Without timestamp queries, fall back to the
    // frame wall time, which overstates GPU cost on a CPU-bound frame; the
    // bottleneck check below is what keeps that from mattering.
    const int64_t costNs = sample.gpuNs > 0 ? sample.gpuNs : sample.frameNs;
    const float load = static_cast<float>(costNs) / static_cast<float>(frameBudgetNs);

    const bool gpuLimited = bottleneck == Bottleneck::Gpu || bottleneck == Bottleneck::Balanced ||
                            bottleneck == Bottleneck::Unknown;

    if ((load > m_config.reduceThreshold || sample.missedDeadline) && gpuLimited)
    {
        if (m_scale > m_config.minScale)
        {
            // Jump straight to the scale the measurement implies instead of
            // stepping down once per dwell: a frame at 200% of budget needs
            // roughly half the pixels now, not in half a second. Cost is
            // close to linear in pixel count, so area scales by 1/load and
            // the linear scale by its square root.
            const float needed = m_scale / std::sqrt(std::max(load / m_config.reduceThreshold, 1.0f));
            const float stepped = m_scale - m_config.step;
            Apply(std::min(stepped, needed));
            decision.changed = true;
        }
        m_dwell = m_config.dwellFrames;
        m_raiseStreak = 0;
    }
    else if (load < m_config.raiseThreshold && !sample.missedDeadline)
    {
        if (++m_raiseStreak >= m_config.raiseConfirmFrames)
        {
            m_raiseStreak = 0;
            if (m_scale < std::min(m_ceiling, m_config.maxScale))
            {
                // One step at a time on the way up. Overshooting means an
                // immediate drop, and the visible flip-flop is worse than
                // taking a second longer to reach native resolution.
                Apply(m_scale + m_config.step);
                decision.changed = true;
                m_dwell = m_config.dwellFrames;
            }
        }
    }
    else
    {
        // In the dead band between the thresholds: hold, and forget partial
        // progress towards a raise so a mixed workload does not creep up.
        m_raiseStreak = 0;
    }

    decision.scale = m_scale;
    decision.width = m_width;
    decision.height = m_height;
    return decision;
}

void ResolutionScaler::Reset()
{
    m_dwell = 0;
    m_raiseStreak = 0;
    Apply(std::min(m_ceiling, m_config.maxScale));
}

} // namespace kisak::perf
