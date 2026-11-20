#pragma once

#include "workbench/audio_frame.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace luciole::workbench {
  // Turns the newest window of mono samples into the AudioFrame of one drawn frame: the spectrum in kAudioBandCount
  // bands from 40 Hz to 10 kHz, the bass as a ratio to its running average, the overall level and the hits. Bands
  // rise fast and fall slowly; a window that is empty, shorter than kAudioWaveformSamples, nearly silent or holds a
  // sample that is not finite reads as silence: no waveform, no bass, level or hit, and bands falling to 0.
  class AudioAnalysis {
  public:
    AudioAnalysis();
    AudioAnalysis(const AudioAnalysis&) = delete;
    AudioAnalysis& operator=(const AudioAnalysis&) = delete;
    AudioAnalysis(AudioAnalysis&&) = delete;
    AudioAnalysis& operator=(AudioAnalysis&&) = delete;
    ~AudioAnalysis() = default;

    // The frame of `samples`, mono at kAudioWaveformRate and oldest first, of which the newest kAudioWaveformSamples
    // are heard, `stepSeconds` after the previous frame. The step is bounded to [0, kMaxAudioFrameSeconds], and 0
    // when it is not a number. The frame's playedSeconds is the start given to reset() plus the bounded steps since,
    // so it falls behind the player's position after a stalled frame; a caller that needs the file's time overwrites
    // it. The frame's views point into this object and stay valid until the next analyze or reset.
    AudioFrame analyze(std::span<const float> samples, double stepSeconds);
    // Forgets everything heard, for a seek or another file: the next frame starts a new playback at `startSeconds`
    // into the file, or at 0 when it is negative or not finite.
    void reset(double startSeconds = 0);

  private:
    static constexpr std::size_t kHalf = kAudioWaveformSamples / 2;

    // The bins a band reads: `count` of them from `bin`, weighed by the entries of m_weights from `weight`.
    struct BandSpan {
      std::size_t weight = 0;
      std::size_t bin = 0;
      std::size_t count = 0;
    };

    bool load(std::span<const float> samples);
    void transform();
    void foldBands(float step);
    void fadeBands(float step);
    float bassFrom(float step);
    bool hitFrom(float step);

    // Fixed tables.
    std::vector<float> m_window;
    std::vector<float> m_cosines; // of 2 pi k / kAudioWaveformSamples, for k below kHalf
    std::vector<float> m_sines;
    std::vector<std::size_t> m_reversed;
    std::vector<float> m_weights;
    std::array<BandSpan, kAudioBandCount> m_bandSpans{};
    std::size_t m_binCount = 0; // bins the bands read
    std::size_t m_hitBands = 0; // the low and middle bands a hit is heard in

    // Per frame.
    std::vector<float> m_waveform;
    std::vector<float> m_real;
    std::vector<float> m_imaginary;
    std::vector<float> m_power;
    std::array<float, kAudioBandCount> m_bandPower{};
    std::array<float, kAudioBandCount> m_bands{};
    double m_meanSquare = 0;

    // Running state.
    float m_bassAverage = 0;
    int m_bassFrames = 0;
    float m_hitAverage = 0;
    int m_hitFrames = 0;
    bool m_hitArmed = true;
    float m_sinceHit = 0;
    double m_playedSeconds = 0;
  };
} // namespace luciole::workbench
