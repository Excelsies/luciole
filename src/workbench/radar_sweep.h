#pragma once

#include "workbench/audio_frame.h"
#include "workbench/audio_history.h"

#include <cstddef>
#include <cstdint>

// The radar visual's scope: a sweep arm that writes the spectrum along its radius into a polar history.
namespace luciole::workbench {
  // The history's texels along a radius and rows around the scope.
  inline constexpr std::size_t kRadarRadii = 128;
  inline constexpr std::size_t kRadarBearings = 192;
  inline constexpr double kRadarTurnsPerSecond = 0.25;
  // The most rows one frame moves the arm, rounded up. A row it steps over takes the echo of the row it writes then,
  // which lies at most this many rows on.
  inline constexpr std::size_t kRadarArmReach = 5;
  static_assert(
      static_cast<double>(kRadarArmReach)
      >= static_cast<double>(kRadarBearings) * kRadarTurnsPerSecond * static_cast<double>(kMaxAudioFrameSeconds)
  );
  // Radii, as a share of the scope's radius, where the lowest and the highest band sit.
  inline constexpr float kRadarInner = 0.06F;
  inline constexpr float kRadarOuter = 0.95F;
  // The gap between the scope's rim and the nearer edge of its box, as a share of the shorter side.
  inline constexpr float kRadarMargin = 0.06F;
  // A hit's brightness along the whole radius, and how fast it fades per second.
  inline constexpr float kRadarSpoke = 0.6F;
  inline constexpr float kRadarKickFade = 30.0F;

  struct RadarScope {
    float centerX = 0;
    float centerY = 0;
    float radius = 0;
  };

  // The largest scope centered in a box of `width` by `height`, with the margin around it. Radius 0 for a box
  // without area or with a side that is not finite.
  RadarScope fitRadarScope(float width, float height);

  // The brightness, 0 to 1, of radius texel `texel`: the bands spread evenly from kRadarInner to kRadarOuter,
  // linear between two bands, with each texel taking the strongest value it spans. 0 for a texel wholly inside the
  // inner radius, past the outer one, or past the last texel.
  float radarEcho(const AudioFrame& frame, std::size_t texel);

  // The arm turns clockwise from north at kRadarTurnsPerSecond. Each advance writes the echo, brightened along the
  // whole radius by a recent hit, into the history row under the arm: red the brightness, green the lap (mod 256)
  // the arm was on, blue how many rows before it the frame that reached the row stepped over (the start row too,
  // on the first frame), and alpha 255 to mark the row as written.
  class RadarSweep {
  public:
    void reset();
    // Turns the arm by the frame's step and writes its row. Throws std::invalid_argument for a history that is not
    // kRadarRadii by kRadarBearings.
    void advance(const AudioFrame& frame, AudioHistory& history);

    // Turns since the start, never decreasing.
    double turns() const { return m_turns; }
    // The arm's bearing as a share of a turn, 0 to 1.
    float bearing() const;
    std::size_t armRow() const;
    int lap() const;
    // The envelope of the latest hit, 1 when it starts and fading toward 0.
    float kick() const { return m_kick; }

  private:
    double m_turns = 0;
    float m_kick = 0;
    std::uint8_t m_stepped = 0;
    bool m_wrote = false;
  };
} // namespace luciole::workbench
