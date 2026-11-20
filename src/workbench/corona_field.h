#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <cstddef>
#include <span>

// The state of the Eclipse sound visual: a dark disc inside a bright ring, with the spectrum wrapped around the ring
// as a corona. Radii and distances are in units of the circle the picture fits (see fitCorona).
namespace luciole::workbench {
  // The disc's radius and the ring's width at rest.
  inline constexpr float kCoronaDiscRadius = 0.44F;
  inline constexpr float kCoronaRingWidth = 0.18F;
  inline constexpr std::size_t kCoronaFlareCount = 4;

  // A ring of light a hit sends outward through the corona.
  struct CoronaFlare {
    float distance = 0;  // past the ring's outer edge, from 0 to 1
    float amplitude = 0; // from 0 to 1; 0 for a slot without a flare
  };

  class CoronaField {
  public:
    // Forgets everything heard: the state of a new field.
    void reset();
    // Moves every level, the swell, the flares and the drift by one frame; fixed work whatever the frame holds.
    void advance(const AudioFrame& frame);

    // One level per band from 0 to 1, lowest band first. The ring shows band 0 at its bottom and the last band at
    // its top, on both sides.
    std::span<const float, kAudioBandCount> levels() const { return m_levels; }
    // The bass drive from 0 to 1, smoothed.
    float swell() const { return m_swell; }
    // The disc's radius, which shrinks a little with the swell.
    float discRadius() const;
    // The ring's outer radius, which moves out with the swell.
    float outerRadius() const;
    std::span<const CoronaFlare, kCoronaFlareCount> flares() const { return m_flares; }
    // The corona's slow turn in radians, from 0 to 2 pi.
    float turn() const { return m_turn; }
    // Seconds of drift for the corona's wandering, from 0 to 1000.
    float clock() const { return m_clock; }

  private:
    void advanceLevels(const AudioFrame& frame, float step);
    void advanceFlares(const AudioFrame& frame, float step);

    std::array<float, kAudioBandCount> m_levels{};
    std::array<CoronaFlare, kCoronaFlareCount> m_flares{};
    float m_swell = 0;
    float m_turn = 0;
    float m_clock = 0;
  };

  // The circle the picture fits, in the units of the rectangle given.
  struct CoronaCircle {
    float centerX = 0;
    float centerY = 0;
    float radius = 1;
  };

  // A circle centered in a rectangle of `width` by `height`, its diameter the shorter side less a margin; a radius of
  // 1 at the origin for a rectangle without area or with a size that is not finite.
  CoronaCircle fitCorona(float width, float height);
} // namespace luciole::workbench
