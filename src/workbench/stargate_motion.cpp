#include "workbench/stargate_motion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace luciole::workbench {
  namespace {
    // How much the roll rate follows the bass, and the time constant of the bass it follows.
    constexpr float kRollBreath = 0.35F;
    constexpr float kRollBassSeconds = 0.8F;
    constexpr float kRollMaxBass = 2;
    // The most a surge adds to the speed, how much it adds per unit of bass above average, and its time constants.
    constexpr float kSurgeMax = 0.8F;
    constexpr float kSurgeGain = 0.8F;
    constexpr float kSurgeRiseSeconds = 0.12F;
    constexpr float kSurgeFallSeconds = 0.7F;
    // The most a hit brightens the picture, and how fast that fades per second.
    constexpr float kPulseMax = 0.35F;
    constexpr float kPulseFade = 6;
    // The glow's time constant, and its drift along the ramp per second.
    constexpr float kGlowSeconds = 0.35F;
    constexpr float kGlowDrift = 0.012F;
    // The fraction kept when more than one row fell due.
    constexpr float kMaxFraction = 0.999F;

    float stepSeconds(float seconds) {
      return std::isnan(seconds) ? 0.0F : std::clamp(seconds, 0.0F, kMaxAudioFrameSeconds);
    }
    float bassOf(float bass) { return std::isnan(bass) ? 0.0F : std::clamp(bass, 0.0F, kMaxAudioBass); }
    // The share of the way to a new value that `seconds` covers with time constant `constant`.
    float approach(float seconds, float constant) { return 1 - std::exp(-seconds / constant); }
  } // namespace

  void StargateRoll::reset() { *this = {}; }

  void StargateRoll::step(float seconds, float bass) {
    const float s = stepSeconds(seconds);
    m_bass += (std::min(bassOf(bass), kRollMaxBass) - m_bass) * approach(s, kRollBassSeconds);
    const float turned = kStargateRollRate * (1 + (kRollBreath * (m_bass - 1))) * s;
    m_angle = std::fmod(m_angle + turned, 2 * std::numbers::pi_v<float>);
  }

  void StargateSurge::reset() { m_surge = 0; }

  float StargateSurge::step(float seconds, float bass) {
    const float s = stepSeconds(seconds);
    const float target = std::clamp((bassOf(bass) - 1) * kSurgeGain, 0.0F, kSurgeMax);
    m_surge += (target - m_surge) * approach(s, target > m_surge ? kSurgeRiseSeconds : kSurgeFallSeconds);
    return value();
  }

  void StargatePulse::reset() { m_kick = 0; }

  float StargatePulse::step(float seconds, bool hit, float bass) {
    if (hit) {
      m_kick = 0.5F + (0.5F * std::min(bassOf(bass), 2.0F) / 2);
    } else {
      m_kick *= std::exp(-stepSeconds(seconds) * kPulseFade);
    }
    return value();
  }

  float StargatePulse::value() const { return 1 + (kPulseMax * m_kick); }

  void StargateSchedule::reset() { *this = {}; }

  bool StargateSchedule::step(float seconds, float rowsPerSecond) {
    const float rate = std::isfinite(rowsPerSecond) ? std::max(rowsPerSecond, 0.0F) : 0.0F;
    const float clock = m_fraction + (stepSeconds(seconds) * rate);
    if (clock < 1) {
      m_fraction = clock;
      return false;
    }
    m_head = (m_head + 1) % kStargateRows;
    m_fraction = std::min(clock - 1, kMaxFraction);
    return true;
  }

  void StargateGlow::reset() { *this = {}; }

  void StargateGlow::step(float seconds, const AudioFrame& frame) {
    const float s = stepSeconds(seconds);
    float sum = 0;
    float moment = 0;
    for (std::size_t band = 0; band < kAudioBandCount; ++band) {
      const float level = spectrumBand(frame, band);
      sum += level;
      moment += level * static_cast<float>(band);
    }
    const float share = approach(s, kGlowSeconds);
    if (sum > 1e-6F) {
      m_hue += ((moment / sum / static_cast<float>(kAudioBandCount - 1)) - m_hue) * share;
    }
    m_energy += ((sum / static_cast<float>(kAudioBandCount)) - m_energy) * share;
    m_drift = std::fmod(m_drift + (s * kGlowDrift), 1.0F);
  }
} // namespace luciole::workbench
