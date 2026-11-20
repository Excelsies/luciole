#pragma once

#include "workbench/audio_frame.h"

#include <cstddef>

// The stargate visual's motion: its roll, its travel speed, its brightness on hits, the clock that pushes history
// rows, and the color of its horizon.
namespace luciole::workbench {
  // Texels across one history row, and rows in the ring.
  inline constexpr std::size_t kStargateLanes = 128;
  inline constexpr std::size_t kStargateRows = 128;
  // Seconds a row takes from the horizon to the eye at the base speed.
  inline constexpr float kStargateTravelSeconds = 3.6F;
  // Radians a second the view rolls at average bass.
  inline constexpr float kStargateRollRate = 0.25F;

  // The roll about the direction of travel: kStargateRollRate, faster on heavy bass and slower in quiet passages, up
  // to 35 % either way, following a slowly smoothed bass so it never jerks.
  class StargateRoll {
  public:
    void reset();
    // Takes `seconds` (clamped to kMaxAudioFrameSeconds) of the bass ratio `bass`.
    void step(float seconds, float bass);
    // Radians, from 0 to 2 pi.
    float angle() const { return m_angle; }

  private:
    float m_angle = 0;
    float m_bass = 1;
  };

  // The travel speed as a multiple of the base speed: 1 up to average bass, up to 1.8 on heavier bass, rising
  // quickly and easing back slowly.
  class StargateSurge {
  public:
    void reset();
    float step(float seconds, float bass);
    float value() const { return 1 + m_surge; }

  private:
    float m_surge = 0;
  };

  // The whole picture's brightness: 1, flaring up to 1.35 on a hit by how heavy the bass is, and fading in about
  // half a second.
  class StargatePulse {
  public:
    void reset();
    float step(float seconds, bool hit, float bass);
    float value() const;

  private:
    float m_kick = 0;
  };

  // The clock that pushes rows into the ring at a rate in rows a second. One step pushes one row at most and drops
  // whatever else fell due, so a long step costs no more than a short one. The fraction of the next push makes the
  // rows glide between pushes.
  class StargateSchedule {
  public:
    void reset();
    // True when a row was pushed: head() then names the ring row that holds it.
    bool step(float seconds, float rowsPerSecond);
    std::size_t head() const { return m_head; }
    // Of the time to the next push, the share already gone, from 0 to below 1.
    float fraction() const { return m_fraction; }

  private:
    std::size_t m_head = 0;
    float m_fraction = 0;
  };

  // The horizon's glow: where on the accent ramp its color sits, from the centroid of the bands, and its level, the
  // mean band, both smoothed; and a slow drift along the ramp.
  class StargateGlow {
  public:
    void reset();
    void step(float seconds, const AudioFrame& frame);
    // 0 for the lowest band to 1 for the highest; 0.5 before any sound.
    float hue() const { return m_hue; }
    float energy() const { return m_energy; }
    // Ramp positions travelled, from 0 to below 1.
    float drift() const { return m_drift; }

  private:
    float m_hue = 0.5F;
    float m_energy = 0;
    float m_drift = 0;
  };
} // namespace luciole::workbench
