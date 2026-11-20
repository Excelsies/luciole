#include "workbench/bar_scene.h"

#include <algorithm>
#include <cmath>

namespace luciole::workbench {
  namespace {
    constexpr std::array<std::size_t, 3> kBarChoices{64, 32, 16};
    constexpr std::size_t kFewestBars = 8;
    constexpr std::size_t kWaveSamples = kWavePoints * kWaveSamplesPerPoint;

    // Shares of the box: the side margins, the base, the lightest point of the bars, a full bar, the waveform line.
    constexpr float kMargin = 0.04F;
    constexpr float kBase = 0.9F;
    constexpr float kTop = 0.08F;
    constexpr float kFullBar = 0.78F;
    constexpr float kWaveCenter = 0.45F;
    constexpr float kWaveAmplitude = 0.18F;
    constexpr float kGapShare = 0.22F;
    // Logical px.
    constexpr float kCapHeight = 3;
    constexpr float kLineWidth = 1.5F;
    constexpr float kGlowWidth = 6;

    // The share of the gap to a lower level kept per 60th of a second, for the bars and for the bass.
    constexpr float kBarKeep = 0.6F;
    constexpr float kBassKeep = 0.9F;
    constexpr float kMaxBass = 2;
    constexpr float kGlowDecayPerSecond = 5;
    // Backdrop brightness: at rest, per unit of the hit glow, per unit of the eased bass.
    constexpr float kRestingBackdrop = 0.25F;
    constexpr float kBackdropPerGlow = 0.35F;
    constexpr float kBackdropPerBass = 0.2F;

    bool positive(float value) { return std::isfinite(value) && value > 0; }
  } // namespace

  std::size_t barCount(float width) {
    if (std::isnan(width)) {
      return kFewestBars;
    }
    const float span = width * (1 - (2 * kMargin));
    for (const std::size_t count : kBarChoices) {
      if (span / static_cast<float>(count) >= kMinBarSlot) {
        return count;
      }
    }
    return kFewestBars;
  }

  BarLayout barLayout(float width, float height, float scale) {
    if (!positive(width) || !positive(height) || !positive(scale)) {
      return {};
    }
    BarLayout layout{.count = barCount(width / scale), .margin = width * kMargin};
    layout.slot = (width - (2 * layout.margin)) / static_cast<float>(layout.count);
    layout.gap = std::max(1.0F, layout.slot * kGapShare);
    layout.base = height * kBase;
    layout.top = height * kTop;
    layout.maxHeight = height * kFullBar;
    layout.capHeight = std::max(2.0F, kCapHeight * scale);
    layout.room = height - layout.base;
    layout.waveCenter = height * kWaveCenter;
    layout.waveAmplitude = height * kWaveAmplitude;
    layout.lineWidth = kLineWidth * scale;
    layout.glowWidth = kGlowWidth * scale;
    return layout;
  }

  void BarScene::reset() {
    m_levels.fill(0);
    m_caps.reset();
    m_wave.fill(0);
    m_glow = 0;
    m_bass = 0;
    m_flash.reset();
  }

  void BarScene::advance(const AudioFrame& frame) {
    const float seconds = frameStep(frame);
    std::array<float, kAudioBandCount> bands{};
    readBands(frame, bands);
    smoothLevels(m_levels, bands, kBarKeep, seconds);
    m_caps.advance(m_levels, seconds);

    // The newest kWaveSamples samples; missing older ones read as 0.
    const std::size_t size = frame.waveform.size();
    const std::size_t first = size > kWaveSamples ? size - kWaveSamples : 0;
    const std::size_t missing = kWaveSamples - std::min(size, kWaveSamples);
    for (std::size_t point = 0; point < kWavePoints; ++point) {
      float sum = 0;
      for (std::size_t k = 0; k < kWaveSamplesPerPoint; ++k) {
        const std::size_t sample = (point * kWaveSamplesPerPoint) + k;
        sum += sample < missing ? 0.0F : waveformSample(frame, first + sample - missing);
      }
      m_wave[point] = sum / static_cast<float>(kWaveSamplesPerPoint);
    }

    m_glow = kickEnvelope(m_glow, hitStrength(frame), kGlowDecayPerSecond, seconds);
    m_bass = easeToward(m_bass, std::min(bassRatio(frame), kMaxBass), kBassKeep, seconds);
    const float wanted = kRestingBackdrop + (kBackdropPerGlow * m_glow) + (kBackdropPerBass * m_bass);
    m_flash.step(std::min(1.0F, wanted), seconds);
  }

  void BarScene::bars(std::span<float> levels, std::span<float> caps) const {
    groupLoudest(m_levels, levels);
    groupLoudest(m_caps.peaks(), caps);
  }
} // namespace luciole::workbench
