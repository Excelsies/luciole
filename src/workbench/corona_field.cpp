#include "workbench/corona_field.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace luciole::workbench {
  namespace {
    // Level time constants in seconds: the corona bursts out quickly and recedes slowly.
    constexpr float kAttackSeconds = 0.05F;
    constexpr float kReleaseSeconds = 0.35F;
    // Bands a level spreads to on each side, a third less per band, so one tone lights a wedge.
    constexpr int kSpread = 2;

    // At full swell the disc shrinks by this share and the ring widens by this share.
    constexpr float kBassShrink = 0.06F;
    constexpr float kBassWiden = 0.25F;
    constexpr float kSwellAttackSeconds = 0.12F;
    constexpr float kSwellReleaseSeconds = 0.4F;

    // Flares travel this many units a second, fade with this time constant, and end past these limits.
    constexpr float kFlareSpeed = 0.5F;
    constexpr float kFlareFadeSeconds = 0.45F;
    constexpr float kFlareEnd = 1.0F;
    constexpr float kFlareMinimum = 0.02F;

    constexpr float kTurnPerSecond = 0.03F;
    constexpr float kClockWrap = 1000.0F;
    constexpr float kMargin = 0.03F;

    // The share of the way to a target one frame of `step` seconds covers with time constant `tau`.
    float approach(float step, float tau) { return 1.0F - std::exp(-step / tau); }
  } // namespace

  void CoronaField::reset() { *this = CoronaField{}; }

  void CoronaField::advance(const AudioFrame& frame) {
    const float step = frameStep(frame);
    advanceLevels(frame, step);
    const float target = std::clamp((bassRatio(frame) - 0.5F) / 1.5F, 0.0F, 1.0F);
    m_swell += (target - m_swell) * approach(step, target > m_swell ? kSwellAttackSeconds : kSwellReleaseSeconds);
    advanceFlares(frame, step);
    m_turn = std::fmod(m_turn + (kTurnPerSecond * step), 2 * std::numbers::pi_v<float>);
    m_clock = std::fmod(m_clock + step, kClockWrap);
  }

  void CoronaField::advanceLevels(const AudioFrame& frame, float step) {
    std::array<float, kAudioBandCount> raw{};
    for (std::size_t i = 0; i < kAudioBandCount; ++i) {
      raw[i] = spectrumBand(frame, i);
    }
    const float attack = approach(step, kAttackSeconds);
    const float release = approach(step, kReleaseSeconds);
    constexpr auto kBands = static_cast<int>(kAudioBandCount);
    for (int i = 0; i < kBands; ++i) {
      float wide = raw[static_cast<std::size_t>(i)];
      for (int k = 1; k <= kSpread; ++k) {
        const float falloff = 1.0F - (static_cast<float>(k) / (kSpread + 1));
        if (i - k >= 0) {
          wide = std::max(wide, raw[static_cast<std::size_t>(i - k)] * falloff);
        }
        if (i + k < kBands) {
          wide = std::max(wide, raw[static_cast<std::size_t>(i + k)] * falloff);
        }
      }
      float& level = m_levels[static_cast<std::size_t>(i)];
      level += (wide - level) * (wide > level ? attack : release);
    }
  }

  void CoronaField::advanceFlares(const AudioFrame& frame, float step) {
    const float fade = std::exp(-step / kFlareFadeSeconds);
    for (auto& flare : m_flares) {
      if (flare.amplitude <= 0) {
        continue;
      }
      flare.distance += kFlareSpeed * step;
      flare.amplitude *= fade;
      if (flare.amplitude < kFlareMinimum || flare.distance > kFlareEnd) {
        flare = {};
      }
    }
    if (!frame.hit) {
      return;
    }
    // A bass three times its average gives the strongest flare.
    const float strength = std::clamp((bassRatio(frame) - 1.0F) / 2.0F, 0.0F, 1.0F);
    // A free slot, else the farthest flare's.
    std::size_t slot = 0;
    for (std::size_t i = 0; i < m_flares.size(); ++i) {
      if (m_flares[i].amplitude <= 0) {
        slot = i;
        break;
      }
      if (m_flares[i].distance > m_flares[slot].distance) {
        slot = i;
      }
    }
    m_flares[slot] = {.distance = 0, .amplitude = 0.35F + (0.65F * strength)};
  }

  float CoronaField::discRadius() const { return kCoronaDiscRadius * (1.0F - (kBassShrink * m_swell)); }

  float CoronaField::outerRadius() const { return discRadius() + (kCoronaRingWidth * (1.0F + (kBassWiden * m_swell))); }

  CoronaCircle fitCorona(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
      return {};
    }
    return {
        .centerX = width / 2,
        .centerY = height / 2,
        .radius = std::max(1.0F, std::min(width, height) / 2 * (1.0F - (2 * kMargin))),
    };
  }
} // namespace luciole::workbench
