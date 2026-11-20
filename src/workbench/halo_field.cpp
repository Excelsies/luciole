#include "workbench/halo_field.h"

#include "workbench/spectrum_meter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace luciole::workbench {
  namespace {
    constexpr float kRestingRadius = 0.2F;
    constexpr float kRadiusPerBass = 0.04F;
    constexpr float kRadiusPerFlash = 0.02F;
    constexpr float kBandReach = 0.12F;
    constexpr float kWobblePerMiddle = 0.006F;
    // The ripple turns this many radians per second.
    constexpr float kRippleSpeed = 1.5F;
    constexpr float kRipplePeriod = 2 * std::numbers::pi_v<float> / kRippleSpeed;

    constexpr float kBandKeep = 0.6F;
    constexpr float kBassKeep = 0.9F;
    constexpr float kMaxBass = 2;
    // The overall level reads as twice itself, so an average song ripples the ring about as much as its bass swells it.
    constexpr float kMiddlePerLevel = 2;
    constexpr float kMaxMiddle = 2;
    constexpr float kKickDecayPerSecond = 6;
  } // namespace

  void HaloField::reset() {
    m_bands.fill(0);
    m_bass = 0;
    m_middle = 0;
    m_clock = 0;
    m_kick = 0;
    m_flash.reset();
  }

  void HaloField::advance(const AudioFrame& frame) {
    const float seconds = frameStep(frame);
    std::array<float, kAudioBandCount> bands{};
    readBands(frame, bands);
    smoothLevels(m_bands, bands, kBandKeep, seconds);
    m_bass = easeToward(m_bass, std::min(bassRatio(frame), kMaxBass), kBassKeep, seconds);
    m_middle = easeToward(m_middle, std::min(audioLevel(frame) * kMiddlePerLevel, kMaxMiddle), kBassKeep, seconds);
    m_clock = std::fmod(m_clock + seconds, kRipplePeriod);
    m_kick = kickEnvelope(m_kick, frame.hit ? 1.0F : 0.0F, kKickDecayPerSecond, seconds);
    m_flash.step(m_kick, seconds);
  }

  HaloRing HaloField::ring() const {
    return {
        .radius = kRestingRadius + (kRadiusPerBass * m_bass) + (kRadiusPerFlash * m_flash.value()),
        .reach = kBandReach,
        .wobble = kWobblePerMiddle * m_middle,
        .phase = std::min(m_clock * kRippleSpeed, std::nextafter(2 * std::numbers::pi_v<float>, 0.0F)),
        .flash = m_flash.value(),
    };
  }

  void HaloField::writeRow(std::span<std::uint8_t> rgba) const {
    const std::size_t texels = std::min(rgba.size() / 4, m_bands.size());
    const std::size_t last = m_bands.size() - 1;
    for (std::size_t texel = 0; texel < texels; ++texel) {
      const float before = m_bands[texel == 0 ? 0 : texel - 1];
      const float after = m_bands[std::min(texel + 1, last)];
      const float softened = (before + (2 * m_bands[texel]) + after) / 4;
      const auto value = static_cast<std::uint8_t>(std::lround(softened * 255));
      const auto out = rgba.subspan(4 * texel, 4);
      out[0] = value;
      out[1] = 0;
      out[2] = 0;
      out[3] = 255;
    }
  }
} // namespace luciole::workbench
