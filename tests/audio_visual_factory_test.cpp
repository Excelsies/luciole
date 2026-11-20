#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual_view.h"

#include <array>
#include <functional>
#include <memory>
#include <print>
#include <string>

namespace {
  using luciole::AudioVisualKind;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;

  constexpr AudioVisualTarget kSmall{.width = 240, .height = 160, .scale = 1};
  constexpr int kFrames = 45;

  using Maker = std::function<std::unique_ptr<AudioVisual>()>;
  // Each kind beside the function its visual's file names, in kind order.
  const std::array<Maker, luciole::kAudioVisualCount> kNamed{
      makeCosmicPeanutVisual, makePulsarVisual,  makeBarsVisual,    makeOrbitVisual,    makeHaloVisual,
      makeRadarVisual,        makeEclipseVisual, makeDarkSunVisual, makeStargateVisual,
  };

  void stepsThroughEveryKind() {
    using K = AudioVisualKind;
    TEST_CHECK(steppedAudioVisual(K::CosmicPeanut, 1) == K::Pulsar);
    TEST_CHECK(steppedAudioVisual(K::Stargate, 1) == K::CosmicPeanut);
    TEST_CHECK(steppedAudioVisual(K::CosmicPeanut, -1) == K::Stargate);
    TEST_CHECK(steppedAudioVisual(K::DarkSun, 0) == K::DarkSun);
    TEST_CHECK(steppedAudioVisual(K::Bars, 9) == K::Bars);
    TEST_CHECK(steppedAudioVisual(K::Bars, -19) == K::Pulsar);
    TEST_CHECK(steppedAudioVisual(static_cast<K>(200), 1) == K::Pulsar);
    // Nine steps forward from each kind visit every kind once.
    K kind = K::Halo;
    std::array<int, luciole::kAudioVisualCount> seen{};
    for (std::size_t i = 0; i < luciole::kAudioVisualCount; ++i) {
      kind = steppedAudioVisual(kind, 1);
      ++seen.at(static_cast<std::size_t>(kind));
    }
    TEST_CHECK(std::ranges::all_of(seen, [](int count) { return count == 1; }));
    TEST_CHECK(makeAudioVisual(static_cast<K>(9)) == nullptr);
    TEST_CHECK(makeAudioVisual(static_cast<K>(255)) == nullptr);
  }

  test::GlPixels picture(AudioVisualRig& rig, AudioVisual& visual) {
    AudioFrameSource source(AudioScene::Kicks);
    const auto capture = rig.run(visual, source, kFrames, kSmall);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    return capture.pixels;
  }

  // Each kind's visual draws exactly the picture of the function named for it, differs from the next kind's, and
  // releases its GL objects without an error.
  void makesEachKind(AudioVisualRig& rig) {
    std::array<test::GlPixels, luciole::kAudioVisualCount> made;
    for (const auto kind : luciole::kAudioVisualKinds) {
      const auto index = static_cast<std::size_t>(kind);
      auto visual = makeAudioVisual(kind);
      TEST_CHECK(visual != nullptr);
      made.at(index) = picture(rig, *visual);
      test::audio::keepForViewing(
          made.at(index), test::audio::captureName(luciole::audioVisualName(kind), "kicks", kSmall)
      );
      auto named = kNamed.at(index)();
      const auto expected = picture(rig, *named);
      const int differing = test::audio::differingPixels(made.at(index), expected, 0);
      std::println("{}: {} pixels differ from its named visual", luciole::audioVisualName(kind), differing);
      TEST_CHECK(differing == 0);
      const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
      TEST_CHECK(
          test::audio::pixelsOtherThan(made.at(index), background, 8, test::audio::wholeRegion(made.at(index))).count
          > 200
      );
      visual->release();
      named->release();
      TEST_CHECK(glGetError() == GL_NO_ERROR);
    }
    for (std::size_t i = 0; i < made.size(); ++i) {
      TEST_CHECK(test::audio::differingPixels(made.at(i), made.at((i + 1) % made.size()), 8) > 200);
    }
  }
} // namespace

int main() {
  stepsThroughEveryKind();
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  makesEachKind(*rig);
  std::println("audio-visual-factory: all checks passed");
  return 0;
}
