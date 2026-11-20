#include "workbench/sun_horizon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace luciole::workbench {
  namespace {
    constexpr float kTwoPi = 2 * std::numbers::pi_v<float>;

    // The range: share of each half over which it tapers to the horizon, depth at its ends against its middle, and
    // the depth of the ridge that remains in silence.
    constexpr float kTaper = 0.3F;
    constexpr float kShoulder = 0.5F;
    constexpr float kRest = 0.1F;
    // Peaks jump in, hold, then sink back slowly.
    constexpr float kPeakAttackSeconds = 0.02F;
    constexpr float kPeakReleaseSeconds = 0.55F;
    constexpr float kPeakHoldSeconds = 0.3F;

    // The sun's swell, its rim from rest to full swell, and its corona's reach in sun radii.
    constexpr float kSwellSeconds = 0.25F;
    constexpr float kRimRest = 0.5F;
    constexpr float kRimFull = 1.0F;
    constexpr float kReachRest = 0.6F;
    constexpr float kReachFull = 1.6F;
    // The rim's brightest point: radians a second at rest, doubled at full energy.
    constexpr float kDiamondPerSecond = 0.12F;
    constexpr float kEnergySeconds = 0.5F;
    constexpr float kCoronaTurnPerSecond = 0.025F;
    // The beam a hit lets fall decays at this rate a second.
    constexpr float kSurgeDecay = 7.0F;
    constexpr float kGlowSeconds = 0.4F;

    // The share of the way to a target one frame of `step` seconds covers with time constant `tau`.
    float approach(float step, float tau) { return 1.0F - std::exp(-step / tau); }

    float wrapAngle(float angle) {
      const float wrapped = std::fmod(angle, kTwoPi);
      return wrapped < 0 ? wrapped + kTwoPi : wrapped;
    }

    // A fixed value in [0, 1) for each key.
    float jag(std::uint32_t key) {
      std::uint32_t x = (key + 1U) * 0x9E3779B9U;
      x ^= x >> 16U;
      x *= 0x85EBCA6BU;
      x ^= x >> 13U;
      return static_cast<float>(x >> 8U) / static_cast<float>(1U << 24U);
    }

    // The column of the left half at `distance` from the middle, and of the right half.
    std::size_t leftColumn(std::size_t distance) { return (kHorizonColumns / 2) - 1 - distance; }
    std::size_t rightColumn(std::size_t distance) { return (kHorizonColumns / 2) + distance; }

    // The range's outline per column: deepest in the middle, kShoulder at the ends before the taper, which is 0 at
    // the outermost columns and eases up to 1 over kTaper of each half.
    std::array<float, kHorizonColumns> envelopeOfRange() {
      std::array<float, kHorizonColumns> out{};
      constexpr float kHalfSpan = (static_cast<float>(kHorizonColumns) / 2) - 0.5F;
      for (std::size_t i = 0; i < kHorizonColumns; ++i) {
        const float middle = static_cast<float>(std::min(i, kHorizonColumns - 1 - i)) / kHalfSpan;
        const float t = std::min(middle / kTaper, 1.0F);
        out[i] = t * t * (3 - (2 * t)) * (kShoulder + ((1 - kShoulder) * middle));
      }
      return out;
    }

    // The ridge in silence, per distance from the middle: jagged, shallower toward the ends.
    std::array<float, kHorizonColumns / 2> restingRidge() {
      constexpr std::size_t kHalf = kHorizonColumns / 2;
      std::array<float, kHalf> out{};
      for (std::size_t k = 0; k < kHalf; ++k) {
        const float fromMiddle = static_cast<float>(k) / static_cast<float>(kHalf);
        out[k] = kRest * (0.35F + (0.65F * jag(static_cast<std::uint32_t>(k)))) * (1 - (0.5F * fromMiddle));
      }
      return out;
    }

    const std::array<float, kHorizonColumns>& rangeEnvelope() {
      static const auto envelope = envelopeOfRange();
      return envelope;
    }

    const std::array<float, kHorizonColumns / 2>& rest() {
      static const auto ridge = restingRidge();
      return ridge;
    }

    // A bass three times its average gives the strongest hit.
    float hitStrength(const AudioFrame& frame) { return std::clamp((bassRatio(frame) - 1.0F) / 2.0F, 0.0F, 1.0F); }
  } // namespace

  std::array<float, kHorizonColumns> SunHorizon::restingProfile() {
    std::array<float, kHorizonColumns> out{};
    for (std::size_t k = 0; k < kHalf; ++k) {
      out[leftColumn(k)] = rangeEnvelope()[leftColumn(k)] * rest()[k];
      out[rightColumn(k)] = rangeEnvelope()[rightColumn(k)] * rest()[k];
    }
    return out;
  }

  void SunHorizon::reset() { *this = SunHorizon{}; }

  void SunHorizon::advance(const AudioFrame& frame) {
    const float step = frameStep(frame);
    advanceRange(frame, step);
    const float bass = bassRatio(frame);
    m_swell += (std::min(0.4F * std::min(bass, 3.0F), 1.0F) - m_swell) * approach(step, kSwellSeconds);
    m_energy += ((std::min(bass, 2.0F) / 2) - m_energy) * approach(step, kEnergySeconds);
    m_diamond = wrapAngle(m_diamond + (kDiamondPerSecond * (1 + m_energy) * step));
    m_coronaTurn = wrapAngle(m_coronaTurn + (kCoronaTurnPerSecond * (0.6F + (0.8F * m_swell)) * step));
    m_surge *= std::exp(-kSurgeDecay * step);
    if (frame.hit) {
      m_surge = std::max(m_surge, 0.5F + (0.5F * hitStrength(frame)));
    }
    m_level += (std::min(2 * audioLevel(frame), 1.0F) - m_level) * approach(step, kGlowSeconds);
  }

  void SunHorizon::advanceRange(const AudioFrame& frame, float step) {
    const float attack = approach(step, kPeakAttackSeconds);
    const float release = approach(step, kPeakReleaseSeconds);
    for (std::size_t k = 0; k < kHalf; ++k) {
      // Two bands per distance from the middle, lowest in the middle; squared, so loud bands stand out as peaks.
      const float band = std::max(spectrumBand(frame, 2 * k), spectrumBand(frame, (2 * k) + 1));
      const float level = band * band;
      float& envelope = m_envelope[k];
      envelope += (level - envelope) * (level > envelope ? attack : release);
      float& peak = m_peak[k];
      if (envelope >= peak) {
        peak = envelope;
        m_peakAge[k] = 0;
      } else {
        m_peakAge[k] = std::min(m_peakAge[k] + step, kPeakHoldSeconds + 1);
        if (m_peakAge[k] > kPeakHoldSeconds) {
          peak += (envelope - peak) * release;
        }
      }
      const float depth = std::max(peak, rest()[k]);
      m_profile[leftColumn(k)] = rangeEnvelope()[leftColumn(k)] * depth;
      m_profile[rightColumn(k)] = rangeEnvelope()[rightColumn(k)] * depth;
    }
  }

  float SunHorizon::rim() const { return kRimRest + ((kRimFull - kRimRest) * m_swell); }

  float SunHorizon::reach() const { return kReachRest + ((kReachFull - kReachRest) * m_swell); }

  float SunHorizon::glow() const { return 0.5F + (0.5F * m_level) + (0.5F * m_surge); }

  SunLayout layoutSun(float width, float height, float swell) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
      return {};
    }
    const float grown = std::isfinite(swell) ? std::clamp(swell, 0.0F, 1.0F) : 0.0F;
    const float unit = std::min(width, height);
    const float horizon = 0.5F * height;
    const float radius = 0.085F * unit * (1 + (grown / 6));
    return {
        .horizon = horizon,
        .sunX = 0.5F * width,
        .sunY = std::max(0.22F * height, 1.2F * radius),
        .sunRadius = radius,
        .rangeHalf = 0.25F * width,
        .rangeDepth = std::min(0.28F * unit, 0.8F * (height - horizon)),
        .beamWidth = 0.004F * unit,
    };
  }
} // namespace luciole::workbench
