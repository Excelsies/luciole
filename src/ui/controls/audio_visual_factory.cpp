#include "ui/controls/audio_visual_view.h"

#include <cstddef>

std::unique_ptr<AudioVisual> makeAudioVisual(luciole::AudioVisualKind kind) {
  using K = luciole::AudioVisualKind;
  switch (kind) {
  case K::CosmicPeanut:
    return makeCosmicPeanutVisual();
  case K::Pulsar:
    return makePulsarVisual();
  case K::Bars:
    return makeBarsVisual();
  case K::Orbit:
    return makeOrbitVisual();
  case K::Halo:
    return makeHaloVisual();
  case K::Radar:
    return makeRadarVisual();
  case K::Eclipse:
    return makeEclipseVisual();
  case K::DarkSun:
    return makeDarkSunVisual();
  case K::Stargate:
    return makeStargateVisual();
  }
  return nullptr;
}

luciole::AudioVisualKind steppedAudioVisual(luciole::AudioVisualKind kind, int steps) {
  constexpr auto kCount = static_cast<long long>(luciole::kAudioVisualCount);
  const auto index = static_cast<long long>(kind);
  const long long from = index < kCount ? index : 0;
  const long long to = (((from + steps) % kCount) + kCount) % kCount;
  return luciole::kAudioVisualKinds.at(static_cast<std::size_t>(to));
}
