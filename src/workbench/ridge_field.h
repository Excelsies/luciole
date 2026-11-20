#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The state behind the Pulsar sound visual: ridge lines stacked into the distance, each new line at the front a
// moment of the sound, its middle raised by the spectrum (bass at the center) and roughened by the waveform, its
// sides flat. Heights are in units of the full peak height.
namespace luciole::workbench {
  inline constexpr std::size_t kRidgePoints = 97;   // points along one line
  inline constexpr std::size_t kRidgeLines = 80;    // lines alive at once
  inline constexpr float kRidgeLinesPerSecond = 10; // how fast new lines come in at the front
  inline constexpr float kRidgeHalfWidth = 0.27F;   // the raised middle's half width, a share of the line

  // A flat top in the middle of the line falling to 0 at kRidgeHalfWidth from the center along a raised cosine,
  // one value per point from the left end to the right end.
  void ridgeEnvelope(std::span<float> out);
  // Band level at a fractional band index, interpolated and clamped to the ends; the first band for a position
  // that is not a number, 0 without bands.
  float bandAt(std::span<const float> bands, float position);

  // A slow automatic gain that brings the lines to a steady peak height without boosting silence past a bound.
  struct RidgeGain {
    float level = 0.5F;

    // Follows a line's raw peak over `seconds`: about a second to rise, five to fall. A value that is not a number
    // reads as 0.
    void step(float peak, float seconds);
    // The factor that brings a line of the followed level to the target height.
    float factor() const;
  };

  // Writes one line into `out` from the bands and the waveform, using `seed` to vary the roughness from line to
  // line, and returns its raw peak before the gain for RidgeGain. `out` and `envelope` hold kRidgePoints values,
  // `bands` kAudioBandCount and `wave` kAudioWaveformSamples, all finite; for other sizes nothing is written and 0
  // is returned.
  float buildRidge(
      std::span<float> out, std::span<const float> envelope, std::span<const float> bands, std::span<const float> wave,
      float gain, std::uint32_t seed
  );
  // The height change per point at `index`: a central difference inside, one-sided at the ends, 0 outside the row.
  float ridgeSlope(std::span<const float> row, std::size_t index);

  // Where the plot goes in a picture, in pixels from its top left corner: centered with margins, at most 0.78 as
  // wide as tall. Baselines run from `top` (the oldest line) to `bottom` (the newest), `spacing` apart; a height
  // of 1 rises `amplitude` above the baseline.
  struct RidgeLayout {
    float left = 0;
    float width = 0;
    float top = 0;
    float bottom = 0;
    float spacing = 0;
    float amplitude = 0;
  };
  // The layout for a picture of this size; all zero for a size that is not positive and finite.
  RidgeLayout ridgeLayout(float width, float height);

  class RidgeField {
  public:
    RidgeField();

    // Back to flat lines that heard nothing.
    void reset();
    // Moves on by the frame's step and pushes the lines that fall due, at most kRidgeLines, from the frame's bands
    // and newest samples.
    void advance(const AudioFrame& frame);

    // kRidgeLines rows of kRidgePoints heights, one line per row; row head() is the newest, at the front.
    std::span<const float> rows() const { return m_rows; }
    std::size_t head() const { return m_head; }
    // Lines pushed since the reset.
    std::uint64_t pushes() const { return m_pushes; }
    // The share of a push interval gone since the last push, 0 to 1: every line is that much further back.
    float fraction() const { return m_fraction; }

  private:
    void push();

    std::vector<float> m_rows;
    std::array<float, kRidgePoints> m_envelope{};
    std::vector<float> m_wave;
    std::array<float, kAudioBandCount> m_bands{};
    std::size_t m_head = 0;
    std::uint64_t m_pushes = 0;
    std::uint32_t m_seed = 0;
    float m_fraction = 0;
    RidgeGain m_gain;
    float m_factor = 1; // the gain the next line is built with
  };
} // namespace luciole::workbench
