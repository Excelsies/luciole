#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// What a sound visual hears once per drawn frame.
namespace luciole::workbench {
  // The waveform window: this many mono samples at this rate, about 93 ms.
  inline constexpr std::size_t kAudioWaveformSamples = 2048;
  inline constexpr int kAudioWaveformRate = 22050;
  inline constexpr std::size_t kAudioBandCount = 64;
  // The producer never reports a longer step between two frames.
  inline constexpr float kMaxAudioFrameSeconds = 0.1F;
  // A bass ratio above this reads as this.
  inline constexpr float kMaxAudioBass = 4.0F;

  // The spans view the producer's buffers and stay valid only during the call that receives the frame: a visual
  // copies what it keeps. Read the fields through the functions below, which keep every value in its range.
  struct AudioFrame {
    // Mono samples from -1 to 1, oldest first and newest last: kAudioWaveformSamples of them at
    // kAudioWaveformRate, or none in silence.
    std::span<const float> waveform;
    // kAudioBandCount spectrum bands from low to high on a logarithmic frequency scale, each 0 to 1; falling to 0
    // in silence.
    std::span<const float> bands;
    // The bass energy as a ratio to its own running average: 1 is average, 0 in silence.
    float bass = 0;
    // The overall level from 0 to 1.
    float level = 0;
    // A hit, a sudden rise of the low and middle bands, started in this frame.
    bool hit = false;
    // Seconds since the previous drawn frame, from 0 to kMaxAudioFrameSeconds.
    float deltaSeconds = 0;
    // Seconds of playback drawn so far, from the start of the file.
    double playedSeconds = 0;
  };

  // Sample `index` of the waveform, clamped to [-1, 1]; 0 past the end and for a value that is not finite.
  inline float waveformSample(const AudioFrame& frame, std::size_t index) {
    const float value = index < frame.waveform.size() ? frame.waveform[index] : 0.0F;
    return std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
  }
  // Band `index`, clamped to [0, 1]; 0 past the end and for a value that is not a number.
  inline float spectrumBand(const AudioFrame& frame, std::size_t index) {
    const float value = index < frame.bands.size() ? frame.bands[index] : 0.0F;
    return std::isnan(value) ? 0.0F : std::clamp(value, 0.0F, 1.0F);
  }
  // The bass ratio, clamped to [0, kMaxAudioBass]; 0 for a value that is not a number.
  inline float bassRatio(const AudioFrame& frame) {
    return std::isnan(frame.bass) ? 0.0F : std::clamp(frame.bass, 0.0F, kMaxAudioBass);
  }
  // The level, clamped to [0, 1]; 0 for a value that is not a number.
  inline float audioLevel(const AudioFrame& frame) {
    return std::isnan(frame.level) ? 0.0F : std::clamp(frame.level, 0.0F, 1.0F);
  }
  // The step since the previous frame, clamped to [0, kMaxAudioFrameSeconds]; 0 for a value that is not a number.
  inline float frameStep(const AudioFrame& frame) {
    return std::isnan(frame.deltaSeconds) ? 0.0F : std::clamp(frame.deltaSeconds, 0.0F, kMaxAudioFrameSeconds);
  }
  // The seconds played; 0 when negative or not finite.
  inline double playbackTime(const AudioFrame& frame) {
    return std::isfinite(frame.playedSeconds) && frame.playedSeconds > 0 ? frame.playedSeconds : 0.0;
  }
} // namespace luciole::workbench
