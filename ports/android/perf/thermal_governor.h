#pragma once

// Thermal management.
//
// Every phone can render this game at 60 fps for about four minutes. The
// interesting question is what happens in the fifth. Left alone, the SoC heats
// until the kernel drops the clocks, and the frame rate falls off a cliff
// partway through a match - which is exactly when it is least acceptable.
//
// The governor watches Android's thermal signals and gives back a little
// performance before the kernel takes a lot. Two inputs:
//
//   * THERMAL_STATUS_* from PowerManager (API 29+): a coarse ladder, updated
//     when the device crosses a threshold.
//   * getThermalHeadroom() (API 30+): a float where 1.0 means "throttling
//     now". Predictive, so it is the primary signal when available.
//
// The output is a ceiling on render scale and a target frame rate, applied by
// the resolution scaler and the pacer. Deliberately asymmetric: it clamps
// immediately on a rise and releases slowly, because thermal mass means a
// device that just cooled below a threshold is still hot.
//
// No Android API in this file; the app layer samples the platform and feeds
// the values in. ports/android/tests/ThermalGovernorTests.cpp drives it.

#include "frame_timing.h"

#include <cstdint>

namespace kisak::perf {

class ThermalGovernor
{
public:
    struct Config
    {
        // Headroom at which to start reducing. Below 1.0 so the clamp lands
        // before the kernel's own throttle does.
        float headroomWarning = 0.85f;
        float headroomCritical = 0.97f;
        // Minimum frames between relaxations, so a brief dip in temperature
        // does not immediately undo a clamp.
        int releaseDwellFrames = 600; // ~10 s at 60 fps
        // Floor on the scale ceiling. Below this the game looks bad enough
        // that dropping the frame rate target is the better trade.
        float minScaleCeiling = 0.55f;
        // Lowest frame rate target the governor will fall back to. 30 is
        // still playable and roughly halves the GPU's energy per second.
        int minTargetFps = 30;
    };

    struct State
    {
        float scaleCeiling = 1.0f;
        int targetFps = 60;
        // True once the governor has taken anything away, so the HUD and the
        // launcher can explain why the game looks softer than it did.
        bool throttling = false;
        ThermalStatus status = ThermalStatus::None;
        float headroom = 0.0f;
    };

    ThermalGovernor();
    explicit ThermalGovernor(const Config &config);

    void Configure(const Config &config);

    // Called when PowerManager reports a new status. Cheap, event-driven.
    void OnThermalStatus(ThermalStatus status);

    // Called periodically (roughly once a second) with getThermalHeadroom().
    // Pass a negative value when the API is unavailable.
    void OnThermalHeadroom(float headroom);

    // Called once per frame. Returns the current ceiling and target.
    State Update();

    const State &state() const { return m_state; }
    void Reset();

private:
    void Clamp(float scaleCeiling, int targetFps);

    Config m_config;
    State m_state;
    int m_dwell = 0;
    // Worst clamp currently in force, independent of the signal that caused
    // it, so a status drop and a headroom spike cannot each undo the other.
    float m_demandedCeiling = 1.0f;
    int m_demandedFps = 60;
};

} // namespace kisak::perf
