#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <cstddef>
#include <numbers>
#include <span>

// The state of the Dark Sun visual: a dark sun that swells with the bass above a horizon, an upside-down mountain
// range hanging from the horizon that is the live spectrum, and a beam of light that falls on every hit.
namespace luciole::workbench {
  // Columns across the range; the lowest bands sit at the middle two and the highest at both ends.
  inline constexpr std::size_t kHorizonColumns = 64;

  class SunHorizon {
  public:
    // Forgets everything heard: the state of a new horizon.
    void reset();
    // Moves the range, the sun, the beam and the drift by one frame; fixed work whatever the frame holds.
    void advance(const AudioFrame& frame);

    // The depth of each column from left to right, from 0 (on the horizon) to 1 (a full peak). Mirrored, tapered to
    // 0 at both ends, and never above a fixed jagged ridge that remains in silence.
    std::span<const float, kHorizonColumns> profile() const { return m_profile; }
    // The bass drive from 0 to 1, smoothed: it grows the sun, its rim light and its corona.
    float swell() const { return m_swell; }
    // The rim's brightness, from 0.5 at rest to 1 at full swell.
    float rim() const;
    // How far the corona reaches past the rim, in sun radii: from 0.6 at rest to 1.6 at full swell.
    float reach() const;
    // The beam's brightness from a hit: 1 at a strong hit, mostly gone after half a second.
    float surge() const { return m_surge; }
    // The horizon's glow: from 0.5 in silence to 1 at full level, plus half the surge.
    float glow() const;
    // The angle of the rim's brightest point in radians, counterclockwise from the right with y up, from 0 to 2 pi.
    float diamond() const { return m_diamond; }
    // The corona's slow turn in radians, from 0 to 2 pi.
    float coronaTurn() const { return m_coronaTurn; }

  private:
    void advanceRange(const AudioFrame& frame, float step);

    static constexpr std::size_t kHalf = kHorizonColumns / 2;
    // Low on the right, as the picture starts.
    static constexpr float kDiamondStart = (2 * std::numbers::pi_v<float>)-0.85F;

    static std::array<float, kHorizonColumns> restingProfile();

    std::array<float, kHorizonColumns> m_profile = restingProfile();
    // Per distance from the middle: the level follower, its held peak and how long the peak has been held.
    std::array<float, kHalf> m_envelope{};
    std::array<float, kHalf> m_peak{};
    std::array<float, kHalf> m_peakAge{};
    float m_swell = 0;
    float m_surge = 0;
    float m_level = 0;
    float m_energy = 0;
    float m_diamond = kDiamondStart;
    float m_coronaTurn = 0;
  };

  // Where the picture's parts sit in a rectangle, in its units with y down from the top edge.
  struct SunLayout {
    float horizon = 0; // the horizon line's y
    float sunX = 0;    // the sun's center
    float sunY = 0;
    float sunRadius = 0;  // the disc's radius
    float rangeHalf = 0;  // half the range's width
    float rangeDepth = 0; // the depth of a full peak below the horizon
    float beamWidth = 0;  // the beam's half width at rest
  };

  // The composition for a rectangle of `width` by `height` and a sun at `swell` (0 to 1): the horizon at half height,
  // the sun centered high in the sky and grown by up to a sixth with the swell, the range half the width across. All
  // zero for a rectangle without area or with a size that is not finite.
  SunLayout layoutSun(float width, float height, float swell);
} // namespace luciole::workbench
