#pragma once

#include "workbench/audio_frame.h"
#include "workbench/hit_flash.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The state behind the Orbit sound visual: sixty-four pillars standing on a ring, one per band, their height
// following the band, a core above the ring's center that swells with the bass, and a camera that circles the ring,
// running ahead and dipping on each hit.
namespace luciole::workbench {
  inline constexpr std::size_t kPillarCount = 64;
  // The ring's radius in world units. Pillar i stands at angle i / 64 turns from +x toward +z, its base on the floor
  // at y = 0.
  inline constexpr float kPillarRingRadius = 4;

  // The band pillar `index` shows: the even bands rise from pillar 0 to pillar 31 and fall back to pillar 63, so the
  // lows face the camera's start, the middle bands the sides, and the highs the far side; 0 past the last pillar.
  std::size_t pillarBand(std::size_t index);
  // A pillar's height for a level from 0 to 1: 0.08 at rest up to 3.28; a level that is not a number reads as 0.
  float pillarHeight(float level);

  // A 4x4 matrix in column-major order.
  using OrbitMatrix = std::array<float, 16>;
  using OrbitPoint = std::array<float, 3>;

  struct OrbitView {
    OrbitMatrix viewProjection{}; // world to clip space
    OrbitPoint eye{};
    OrbitPoint right{}; // the camera's right and up, unit length in world space
    OrbitPoint up{};
  };

  // The pillars ordered from the farthest from `eye` on the floor to the nearest, ties by index: an order to paint
  // them in without a depth buffer.
  std::array<std::uint8_t, kPillarCount> drawOrder(const OrbitPoint& eye);
  // How many pillars stand farther from `eye` on the floor than the ring's center, where the core floats: the first
  // of drawOrder(eye) to paint before the core.
  std::size_t pillarsBehindCore(const OrbitPoint& eye);

  class PillarRing {
  public:
    void reset();
    void advance(const AudioFrame& frame);
    // Each pillar's level, from 0 to 1.
    std::span<const float> levels() const { return m_levels; }
    // From 1 on a hit down to 0.
    float pulse() const { return m_flash.value(); }
    // The core's radius: 0.7 in silence, swelling with the bass up to 1.4.
    float coreRadius() const;
    // The core's turns about the vertical and the horizontal axis, in radians from 0 to 2 pi.
    std::array<float, 2> coreTurn() const { return m_coreTurn; }
    // The star field's turn about the vertical axis, in radians from -2 pi to 0.
    float starTurn() const { return m_starTurn; }
    // The camera for a box of `aspect` width over height: a vertical field of 50 degrees, widened in a tall box so
    // the ring keeps a horizontal field of 50 degrees. An aspect that is not a positive number reads as 1.
    OrbitView view(float aspect) const;

  private:
    std::array<float, kPillarCount> m_levels{};
    float m_bass = 0;
    float m_kick = 0;
    FlashLimiter m_flash;
    float m_orbit = 0;
    std::array<float, 2> m_coreTurn{};
    float m_starTurn = 0;
  };
} // namespace luciole::workbench
