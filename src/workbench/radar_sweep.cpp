#include "workbench/radar_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr auto kLastBand = static_cast<float>(kAudioBandCount - 1);
    constexpr std::uint8_t kWritten = 255;

    // The radii a texel covers inside the banded part of the scope, none when it lies outside.
    std::optional<std::pair<float, float>> texelSpan(std::size_t texel) {
      if (texel >= kRadarRadii) {
        return std::nullopt;
      }
      constexpr auto kCount = static_cast<float>(kRadarRadii);
      const float inner = std::max(kRadarInner, static_cast<float>(texel) / kCount);
      const float outer = std::min(kRadarOuter, static_cast<float>(texel + 1) / kCount);
      if (outer <= inner) {
        return std::nullopt;
      }
      return std::pair{inner, outer};
    }

    float bandPosition(float radius) { return (radius - kRadarInner) / (kRadarOuter - kRadarInner) * kLastBand; }

    // The spectrum at a fractional band position, linear between two bands.
    float bandAt(const AudioFrame& frame, float position) {
      if (std::isnan(position) || position < 0 || position > kLastBand) {
        return 0;
      }
      const auto low = std::min(static_cast<std::size_t>(position), kAudioBandCount - 2);
      const float a = spectrumBand(frame, low);
      const float b = spectrumBand(frame, low + 1);
      return a + ((b - a) * (position - static_cast<float>(low)));
    }

    std::uint8_t toByte(float value) {
      return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    }
  } // namespace

  RadarScope fitRadarScope(float width, float height) {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
      return {};
    }
    return {
        .centerX = width / 2,
        .centerY = height / 2,
        .radius = std::min(width, height) / 2 * (1 - (2 * kRadarMargin)),
    };
  }

  float radarEcho(const AudioFrame& frame, std::size_t texel) {
    const auto span = texelSpan(texel);
    if (!span) {
      return 0;
    }
    const float first = bandPosition(span->first);
    const float last = bandPosition(span->second);
    float value = std::max(bandAt(frame, first), bandAt(frame, last));
    const auto from = static_cast<std::size_t>(std::ceil(first));
    const auto to = std::min(static_cast<std::size_t>(last), kAudioBandCount - 1);
    for (std::size_t band = from; band <= to; ++band) {
      value = std::max(value, spectrumBand(frame, band));
    }
    return value;
  }

  void RadarSweep::reset() {
    m_turns = 0;
    m_kick = 0;
    m_stepped = 0;
    m_wrote = false;
  }

  void RadarSweep::advance(const AudioFrame& frame, AudioHistory& history) {
    if (history.width() != kRadarRadii || history.rows() != kRadarBearings) {
      throw std::invalid_argument("the radar history must be 128 by 192 texels");
    }
    const float step = frameStep(frame);
    const std::size_t from = armRow();
    m_turns += static_cast<double>(step) * kRadarTurnsPerSecond;
    m_kick = frame.hit ? 1.0F : m_kick * std::exp(-step * kRadarKickFade);
    const std::size_t to = armRow();
    if (to != from) {
      const std::size_t moved = (to + kRadarBearings - from) % kRadarBearings;
      m_stepped = static_cast<std::uint8_t>(std::min(m_wrote ? moved - 1 : moved, kRadarArmReach));
    }
    m_wrote = true;
    const auto row = history.writeRow(to);
    const auto lapByte = static_cast<std::uint8_t>(lap());
    for (std::size_t texel = 0; texel < kRadarRadii; ++texel) {
      const float spoke = texelSpan(texel) ? m_kick * kRadarSpoke : 0.0F;
      const std::size_t at = texel * AudioHistory::kBytesPerTexel;
      row[at] = toByte(std::max(radarEcho(frame, texel), spoke));
      row[at + 1] = lapByte;
      row[at + 2] = m_stepped;
      row[at + 3] = kWritten;
    }
  }

  float RadarSweep::bearing() const { return static_cast<float>(m_turns - std::floor(m_turns)); }

  std::size_t RadarSweep::armRow() const {
    const double share = m_turns - std::floor(m_turns);
    return std::min(static_cast<std::size_t>(share * static_cast<double>(kRadarBearings)), kRadarBearings - 1);
  }

  int RadarSweep::lap() const { return static_cast<int>(std::fmod(std::floor(m_turns), 256.0)); }
} // namespace luciole::workbench
