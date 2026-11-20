#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The state behind the Cosmic Peanut sound visual: waveform rings pushed at a fixed rate onto an implied sphere
// of radius 1, newest at the north pole, a bass pulse that swells the form, and a camera that turns by itself.
namespace luciole::workbench {
  inline constexpr std::size_t kRingPoints = 128;      // points around one ring
  inline constexpr std::size_t kRingCount = 96;        // rings alive at once
  inline constexpr std::size_t kRingWindow = 203;      // waveform samples one ring spans, about 9 ms
  inline constexpr float kRingTravelSeconds = 7;       // from the north pole to the south pole
  inline constexpr float kRingAmplitude = 0.22F;       // how far a sample of 1 pushes a point along the normal
  inline constexpr float kRingPeakDecay = 0.992F;      // the tracked peak's decay per ring
  inline constexpr float kRingPeakFloor = 0.02F;       // the least peak a ring is divided by
  inline constexpr float kRingClamp = 1.5F;            // a finished sample stays within this either side of 0
  inline constexpr float kRingTaper = 0.08F;           // share of a ring at each end that eases to 0
  inline constexpr float kRingPulseMax = 1.5F;         // the highest smoothed pulse
  inline constexpr float kRingSwell = 0.021F;          // radius growth per unit of pulse
  inline constexpr float kRingFieldOfView = 0.663225F; // vertical, 38 degrees

  // Where the ring window starts in `samples`, at sub-sample precision: the first rising zero crossing that leaves
  // a full window after it. 0 when there is none, when `window` is 0, or when `samples` holds fewer than twice the
  // window.
  double ringStart(std::span<const float> samples, std::size_t window);
  // Point-samples `window` samples from `start` into `out` with linear interpolation, reading the last sample past
  // the end, and returns the largest magnitude written. An empty `samples` gives a flat ring.
  float resampleRing(std::span<const float> samples, double start, std::size_t window, std::span<float> out);
  // The tracked peak after a ring whose peak is `peak`: a louder ring at once, otherwise a slow decay.
  float nextRingPeak(float tracked, float peak);
  // Divides `ring` by `tracked` (at least kRingPeakFloor), eases both ends to 0 with a raised cosine so the ring
  // closes without a seam, and clamps to kRingClamp.
  void finishRing(std::span<float> ring, float tracked);

  // Where the pulse heads for a bass ratio (1 is average): 0 to kRingPulseMax; 0 for a value that is not a number.
  float ringPulseTarget(float bass);
  // The pulse after `seconds` heading for the target of `bass`: quick to rise, slow to fall.
  float stepRingPulse(float pulse, float bass, float seconds);
  // The radius factor of the whole form at `pulse`.
  float ringSwell(float pulse);

  // Radians around the vertical axis and above the equator, and the clock of the slow pitch sway.
  struct RingCamera {
    float yaw = 0.4F;
    float pitch = 0.42F;
    float sway = 0;
  };
  // The camera after `seconds` of turning by itself.
  RingCamera stepRingCamera(const RingCamera& camera, float seconds);

  // Column-major matrices for a GL program, and the eye's distance from the sphere's center.
  struct RingView {
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    float distance = 0;
  };
  // The view of `camera` for a picture `aspect` wide per unit of height, far enough to keep the fully swollen
  // sphere inside also when the picture is taller than wide, with the far plane just past it. An aspect that is not
  // positive and finite reads as 1.
  RingView ringView(const RingCamera& camera, float aspect);

  class RingField {
  public:
    RingField();

    // Back to a field that heard nothing.
    void reset();
    // Moves on by the frame's step: pushes the rings that fall due, at most kRingCount, each from the frame's
    // newest samples; steps the pulse and the camera.
    void advance(const AudioFrame& frame);

    // kRingCount rows of kRingPoints samples, one ring per row; row head() is the newest.
    std::span<const float> rows() const { return m_rows; }
    std::size_t head() const { return m_head; }
    // Rows written since the reset, at most kRingCount.
    std::size_t count() const { return m_count; }
    // Rings pushed since the reset.
    std::uint64_t pushes() const { return m_pushes; }
    // The share of a push interval gone since the last push, 0 to 1: every ring is that much further south.
    float fraction() const { return m_fraction; }
    float pulse() const { return m_pulse; }
    const RingCamera& camera() const { return m_camera; }

  private:
    void push(const AudioFrame& frame);

    std::vector<float> m_rows;
    std::array<float, 2 * kRingWindow> m_window{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    std::uint64_t m_pushes = 0;
    float m_fraction = 0;
    float m_tracked = 0.1F;
    float m_pulse = 0;
    RingCamera m_camera;
  };
} // namespace luciole::workbench
