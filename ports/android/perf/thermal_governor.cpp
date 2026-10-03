#include "thermal_governor.h"

#include <algorithm>
#include <cmath>

namespace kisak::perf {

ThermalGovernor::ThermalGovernor()
{
    Configure(Config{});
}

ThermalGovernor::ThermalGovernor(const Config &config)
{
    Configure(config);
}

void ThermalGovernor::Configure(const Config &config)
{
    m_config = config;
    m_config.headroomWarning = std::clamp(m_config.headroomWarning, 0.5f, 1.0f);
    m_config.headroomCritical = std::clamp(m_config.headroomCritical, m_config.headroomWarning + 0.01f, 1.2f);
    m_config.releaseDwellFrames = std::max(1, m_config.releaseDwellFrames);
    m_config.minScaleCeiling = std::clamp(m_config.minScaleCeiling, 0.4f, 1.0f);
    m_config.minTargetFps = std::clamp(m_config.minTargetFps, 20, 60);
    Reset();
}

void ThermalGovernor::Reset()
{
    m_state = State{};
    m_dwell = 0;
    m_demandedCeiling = 1.0f;
    m_demandedFps = 60;
}

void ThermalGovernor::Clamp(float scaleCeiling, int targetFps)
{
    // Only ever tighten here. Release happens in Update(), on a timer.
    const bool tighter = scaleCeiling < m_demandedCeiling - 1e-4f || targetFps < m_demandedFps;
    if (!tighter)
        return;
    m_demandedCeiling = std::min(m_demandedCeiling, scaleCeiling);
    m_demandedFps = std::min(m_demandedFps, targetFps);
    m_dwell = m_config.releaseDwellFrames;
}

void ThermalGovernor::OnThermalStatus(ThermalStatus status)
{
    m_state.status = status;
    switch (status)
    {
    case ThermalStatus::None:
        break;
    case ThermalStatus::Light:
        // Barely warm. Give up a little resolution, keep 60 fps: the player
        // should not be able to tell this happened.
        Clamp(0.90f, 60);
        break;
    case ThermalStatus::Moderate:
        // The kernel is about to start throttling. Resolution first, frame
        // rate second - a soft image at 60 beats a sharp one at 45.
        Clamp(0.75f, 60);
        break;
    case ThermalStatus::Severe:
        Clamp(m_config.minScaleCeiling, 45);
        break;
    case ThermalStatus::Critical:
    case ThermalStatus::Emergency:
    case ThermalStatus::Shutdown:
        // At this point the platform may kill the app. Shed everything that
        // can be shed and stay running.
        Clamp(m_config.minScaleCeiling, m_config.minTargetFps);
        break;
    }
}

void ThermalGovernor::OnThermalHeadroom(float headroom)
{
    if (!(headroom >= 0.0f) || !std::isfinite(headroom))
        return; // API unavailable on this device/level
    m_state.headroom = headroom;

    if (headroom >= m_config.headroomCritical)
    {
        Clamp(m_config.minScaleCeiling, 45);
    }
    else if (headroom >= m_config.headroomWarning)
    {
        // Scale the ceiling linearly across the warning band, so the picture
        // softens gradually instead of stepping at one threshold.
        const float t = (headroom - m_config.headroomWarning) /
                        std::max(0.01f, m_config.headroomCritical - m_config.headroomWarning);
        const float ceiling = 1.0f - t * (1.0f - m_config.minScaleCeiling);
        Clamp(ceiling, 60);
    }
}

ThermalGovernor::State ThermalGovernor::Update()
{
    if (m_dwell > 0)
    {
        --m_dwell;
    }
    else if (m_demandedCeiling < 1.0f || m_demandedFps < 60)
    {
        // Cool enough for long enough: hand a little back. One step at a
        // time, and the dwell restarts, so a full recovery from the deepest
        // clamp takes minutes - which matches how long the device needs to
        // actually shed the heat.
        const bool stillHot = m_state.status >= ThermalStatus::Moderate ||
                              (m_state.headroom > 0.0f && m_state.headroom >= m_config.headroomWarning);
        if (!stillHot)
        {
            if (m_demandedFps < 60)
                m_demandedFps = std::min(60, m_demandedFps + 15);
            else
                m_demandedCeiling = std::min(1.0f, m_demandedCeiling + 0.05f);
            m_dwell = m_config.releaseDwellFrames;
        }
        else
        {
            m_dwell = m_config.releaseDwellFrames;
        }
    }

    m_state.scaleCeiling = std::clamp(m_demandedCeiling, m_config.minScaleCeiling, 1.0f);
    m_state.targetFps = std::clamp(m_demandedFps, m_config.minTargetFps, 60);
    m_state.throttling = m_state.scaleCeiling < 0.999f || m_state.targetFps < 60;
    return m_state;
}

} // namespace kisak::perf
