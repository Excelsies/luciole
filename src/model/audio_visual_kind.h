#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace luciole {
  // The visual a sound preview shows while it plays.
  enum class AudioVisualKind : std::uint8_t {
    CosmicPeanut,
    Pulsar,
    Bars,
    Orbit,
    Halo,
    Radar,
    Eclipse,
    DarkSun,
    Stargate
  };
  inline constexpr std::size_t kAudioVisualCount = 9;
  inline constexpr std::array<AudioVisualKind, kAudioVisualCount> kAudioVisualKinds{
      AudioVisualKind::CosmicPeanut, AudioVisualKind::Pulsar,  AudioVisualKind::Bars,
      AudioVisualKind::Orbit,        AudioVisualKind::Halo,    AudioVisualKind::Radar,
      AudioVisualKind::Eclipse,      AudioVisualKind::DarkSun, AudioVisualKind::Stargate,
  };
  // The name shown to the user, in kind order.
  inline constexpr std::array<std::string_view, kAudioVisualCount> kAudioVisualLabels{
      "Cosmic Peanut", "Pulsar", "Bars", "Orbit", "Halo", "Radar", "Eclipse", "Dark Sun", "Stargate"
  };
  // The word in the settings file, in kind order.
  inline constexpr std::array<std::string_view, kAudioVisualCount> kAudioVisualNames{
      "cosmic-peanut", "pulsar", "bars", "orbit", "halo", "radar", "eclipse", "dark-sun", "stargate"
  };
  // Empty for a value outside the enum.
  constexpr std::string_view audioVisualLabel(AudioVisualKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < kAudioVisualLabels.size() ? kAudioVisualLabels[index] : std::string_view{};
  }
  constexpr std::string_view audioVisualName(AudioVisualKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < kAudioVisualNames.size() ? kAudioVisualNames[index] : std::string_view{};
  }
  // The kind whose settings word is exactly `name`; none for any other text.
  constexpr std::optional<AudioVisualKind> audioVisualFromName(std::string_view name) {
    for (const auto kind : kAudioVisualKinds) {
      if (audioVisualName(kind) == name) {
        return kind;
      }
    }
    return std::nullopt;
  }
} // namespace luciole
