#pragma once

#include "workbench/audio_frame.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

// Audio frames generated without any analysis, for the tests of sound visuals and of the logic behind them.
namespace test::audio {
  using luciole::workbench::AudioFrame;

  inline constexpr int kFramesPerSecond = 60;
  inline constexpr double kToneHz = 440;
  inline constexpr float kToneAmplitude = 0.6F;
  inline constexpr std::size_t kToneBand = 30;
  inline constexpr int kKickFrames = kFramesPerSecond / 2; // a kick every half second
  inline constexpr std::size_t kKickBands = 8;             // the lowest bands a kick fills

  // Tone: a steady sine of kToneHz, band kToneBand high and the others 0, bass 1. Noise: white noise in the
  // waveform and every band between 0.25 and 0.75. Kicks: a decaying 55 Hz thump every kKickFrames frames, filling
  // the lowest kKickBands bands, bass about 3 at the hit and below 1 between hits, the hit flag on the first frame of
  // each. Silence: no waveform, every band 0. Hostile: frames that break every promise of AudioFrame in turn.
  enum class AudioScene : std::uint8_t { Tone, Noise, Kicks, Silence, Hostile };

  // Frames of one scene at kFramesPerSecond, the same for every source of that scene.
  class AudioFrameSource {
  public:
    explicit AudioFrameSource(AudioScene scene) : m_scene(scene) {}

    // The next frame; its views stay valid until the next call or until the source goes.
    AudioFrame next() {
      const int index = m_frames++;
      AudioFrame frame{
          .waveform = {},
          .bands = {},
          .deltaSeconds = 1.0F / kFramesPerSecond,
          .playedSeconds = static_cast<double>(index) / kFramesPerSecond,
      };
      m_bands.assign(luciole::workbench::kAudioBandCount, 0.0F);
      m_waveform.assign(luciole::workbench::kAudioWaveformSamples, 0.0F);
      switch (m_scene) {
      case AudioScene::Tone:
        tone(frame);
        break;
      case AudioScene::Noise:
        noise(index, frame);
        break;
      case AudioScene::Kicks:
        kicks(index, frame);
        break;
      case AudioScene::Silence:
        m_waveform.clear();
        break;
      case AudioScene::Hostile:
        hostile(index, frame);
        break;
      }
      frame.waveform = m_waveform;
      frame.bands = m_bands;
      return frame;
    }

    int frames() const { return m_frames; }

  private:
    // Seconds of the waveform sample at `offset` of the window ending at `played`.
    static double sampleTime(double played, std::size_t offset) {
      const auto newest = std::floor(played * luciole::workbench::kAudioWaveformRate);
      const auto back = static_cast<double>(luciole::workbench::kAudioWaveformSamples - 1 - offset);
      return (newest - back) / luciole::workbench::kAudioWaveformRate;
    }

    // A fixed pseudo-random value in [0, 1) for each key.
    static float hashed(std::int64_t key) {
      auto x = static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ULL;
      x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
      x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
      return static_cast<float>((x ^ (x >> 31U)) >> 40U) / static_cast<float>(1U << 24U);
    }

    void tone(AudioFrame& frame) {
      for (std::size_t i = 0; i < m_waveform.size(); ++i) {
        const double phase = 2 * std::numbers::pi * kToneHz * sampleTime(frame.playedSeconds, i);
        m_waveform[i] = kToneAmplitude * static_cast<float>(std::sin(phase));
      }
      m_bands[kToneBand] = 0.9F;
      frame.bass = 1;
      frame.level = kToneAmplitude * std::numbers::sqrt2_v<float> / 2;
    }

    void noise(int index, AudioFrame& frame) {
      for (std::size_t i = 0; i < m_waveform.size(); ++i) {
        const auto sample = std::llround(sampleTime(frame.playedSeconds, i) * luciole::workbench::kAudioWaveformRate);
        m_waveform[i] = hashed(sample) - 0.5F;
      }
      for (std::size_t band = 0; band < m_bands.size(); ++band) {
        m_bands[band] = 0.25F + (0.5F * hashed((std::int64_t{index} << 8) + static_cast<std::int64_t>(band) + 1));
      }
      frame.bass = 0.8F + (0.4F * hashed(-1 - index));
      frame.level = 0.29F;
    }

    void kicks(int index, AudioFrame& frame) {
      constexpr double kPeriod = static_cast<double>(kKickFrames) / kFramesPerSecond;
      constexpr double kDecay = 0.08;
      for (std::size_t i = 0; i < m_waveform.size(); ++i) {
        double age = std::fmod(sampleTime(frame.playedSeconds, i), kPeriod);
        age = age < 0 ? age + kPeriod : age;
        const double thump = std::exp(-age / kDecay) * std::sin(2 * std::numbers::pi * 55 * age);
        m_waveform[i] = 0.9F * static_cast<float>(thump);
      }
      const auto envelope =
          static_cast<float>(std::exp(-static_cast<double>(index % kKickFrames) / kFramesPerSecond / kDecay));
      for (std::size_t band = 0; band < kKickBands; ++band) {
        m_bands[band] = envelope * (1.0F - (static_cast<float>(band) / kKickBands));
      }
      frame.bass = 0.4F + (2.6F * envelope);
      frame.level = 0.6F * envelope;
      frame.hit = index % kKickFrames == 0;
    }

    static void setValues(AudioFrame& frame, float bass, float level, float step, double played) {
      frame.bass = bass;
      frame.level = level;
      frame.hit = true;
      frame.deltaSeconds = step;
      frame.playedSeconds = played;
    }

    void hostile(int index, AudioFrame& frame) {
      constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
      constexpr float kInfinity = std::numeric_limits<float>::infinity();
      switch (index % 4) {
      case 0:
        for (std::size_t i = 0; i < m_waveform.size(); ++i) {
          m_waveform[i] = i % 7 == 0 ? kNan : (i % 2 == 0 ? 5.0F : -5.0F);
        }
        std::ranges::fill(m_bands, kNan);
        m_bands[1] = -1;
        m_bands[2] = 3;
        setValues(frame, kNan, kNan, kNan, std::numeric_limits<double>::quiet_NaN());
        break;
      case 1:
        m_waveform.assign(5000, 1e30F);
        m_waveform[1] = -kInfinity;
        m_bands.assign(3, kInfinity);
        setValues(frame, kInfinity, 2, 1e6F, 1e12);
        break;
      case 2:
        m_waveform.clear();
        m_bands.clear();
        setValues(frame, -1, -1, -1, -5);
        break;
      default:
        m_waveform.assign(1, 1);
        m_bands.assign(200, 1e9F);
        setValues(frame, 1e9F, kInfinity, 0, std::numeric_limits<double>::infinity());
        break;
      }
    }

    AudioScene m_scene;
    int m_frames = 0;
    std::vector<float> m_waveform;
    std::vector<float> m_bands;
  };
} // namespace test::audio
