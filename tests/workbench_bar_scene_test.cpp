#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/bar_scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>

namespace {
  using namespace luciole::workbench;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  void barsMergeInNarrowBoxes() {
    TEST_CHECK(barCount(900) == 64);
    TEST_CHECK(barCount(450) == 32);
    TEST_CHECK(barCount(200) == 16);
    TEST_CHECK(barCount(100) == 8);
    TEST_CHECK(barCount(0) == 8);
    TEST_CHECK(barCount(-50) == 8);
    TEST_CHECK(barCount(std::numeric_limits<float>::quiet_NaN()) == 8);
    TEST_CHECK(barCount(std::numeric_limits<float>::infinity()) == 64);
  }

  void layoutFollowsTheBox() {
    const BarLayout layout = barLayout(450, 600, 1);
    TEST_CHECK(layout.count == 32);
    TEST_NEAR(layout.margin, 18.0F, 1e-4F);
    TEST_NEAR(layout.slot, 12.9375F, 1e-4F);
    TEST_NEAR(layout.gap, 2.84625F, 1e-4F);
    TEST_NEAR(layout.base, 540.0F, 1e-4F);
    TEST_NEAR(layout.top, 48.0F, 1e-4F);
    TEST_NEAR(layout.maxHeight, 468.0F, 1e-4F);
    TEST_NEAR(layout.capHeight, 3.0F, 1e-4F);
    TEST_NEAR(layout.room, 60.0F, 1e-4F);
    TEST_NEAR(layout.waveCenter, 270.0F, 1e-4F);
    TEST_NEAR(layout.waveAmplitude, 108.0F, 1e-4F);
    TEST_NEAR(layout.lineWidth, 1.5F, 1e-4F);
    TEST_NEAR(layout.glowWidth, 6.0F, 1e-4F);
    // The count follows the logical width; strokes and caps follow the scale.
    const BarLayout sharp = barLayout(1350, 750, 1.5F);
    TEST_CHECK(sharp.count == 64);
    TEST_NEAR(sharp.capHeight, 4.5F, 1e-4F);
    TEST_NEAR(sharp.lineWidth, 2.25F, 1e-4F);
    TEST_NEAR(sharp.base, 675.0F, 1e-4F);
    // A narrow gap is still a whole pixel.
    TEST_NEAR(barLayout(30, 100, 1).gap, 1.0F, 1e-4F);
    for (const auto& empty :
         {barLayout(0, 600, 1), barLayout(450, -1, 1), barLayout(450, 600, 0),
          barLayout(std::numeric_limits<float>::quiet_NaN(), 600, 1), barLayout(450, 600, -2)}) {
      TEST_CHECK(empty.count == 0);
      TEST_NEAR(empty.slot, 0.0F, 1e-6F);
      TEST_NEAR(empty.maxHeight, 0.0F, 1e-6F);
    }
  }

  BarScene played(AudioScene scene, int frames) {
    BarScene bars;
    AudioFrameSource source(scene);
    for (int frame = 0; frame < frames; ++frame) {
      bars.advance(source.next());
    }
    return bars;
  }

  void aToneLiftsItsBar() {
    const BarScene bars = played(AudioScene::Tone, 60);
    std::array<float, 32> levels{};
    std::array<float, 32> caps{};
    bars.bars(levels, caps);
    // Band 30 falls in bar 15 of 32.
    TEST_NEAR(levels[15], 0.9F, 1e-4F);
    TEST_NEAR(caps[15], 0.9F, 1e-4F);
    TEST_NEAR(levels[14], 0.0F, 1e-6F);
    TEST_NEAR(levels[16], 0.0F, 1e-6F);
    std::array<float, 8> fewer{};
    std::array<float, 8> fewerCaps{};
    bars.bars(fewer, fewerCaps);
    TEST_NEAR(fewer[3], 0.9F, 1e-4F);
    TEST_NEAR(fewer[2], 0.0F, 1e-6F);
  }

  void theWaveShowsTheNewestSamples() {
    const BarScene bars = played(AudioScene::Tone, 10);
    const auto wave = bars.wave();
    TEST_CHECK(wave.size() == kWavePoints);
    int crossings = 0;
    for (std::size_t i = 1; i < wave.size(); ++i) {
      crossings += (wave[i - 1] < 0) != (wave[i] < 0) ? 1 : 0;
    }
    // 512 samples at 22050 Hz hold about ten periods of 440 Hz.
    TEST_CHECK(crossings >= 19 && crossings <= 22);
    TEST_NEAR(*std::ranges::max_element(wave), 0.6F, 0.02F);
    TEST_NEAR(*std::ranges::min_element(wave), -0.6F, 0.02F);
    const BarScene quiet = played(AudioScene::Silence, 10);
    TEST_CHECK(std::ranges::all_of(quiet.wave(), [](float y) { return y == 0.0F; }));
  }

  void kicksJumpThenFallWhileCapsHold() {
    BarScene bars;
    AudioFrameSource source(AudioScene::Kicks);
    std::array<float, 32> levels{};
    std::array<float, 32> caps{};
    bars.advance(source.next());
    bars.bars(levels, caps);
    TEST_NEAR(levels[0], 1.0F, 1e-4F);
    const float struck = bars.backdrop();
    for (int frame = 1; frame < 26; ++frame) {
      bars.advance(source.next());
    }
    bars.bars(levels, caps);
    TEST_CHECK(levels[0] < 0.1F);
    TEST_NEAR(caps[0], 1.0F, 1e-4F);
    TEST_CHECK(bars.backdrop() < struck - 0.2F);
    for (int frame = 26; frame < 30; ++frame) {
      bars.advance(source.next());
    }
    bars.bars(levels, caps);
    TEST_CHECK(caps[0] < 1.0F && caps[0] >= levels[0]);
    bars.advance(source.next());
    bars.bars(levels, caps);
    TEST_NEAR(levels[0], 1.0F, 1e-4F);
    TEST_NEAR(bars.backdrop(), struck, 0.15F);
    TEST_CHECK(struck > 0.6F);
  }

  void silenceSettlesCalm() {
    const BarScene bars = played(AudioScene::Silence, 120);
    std::array<float, 64> levels{};
    std::array<float, 64> caps{};
    bars.bars(levels, caps);
    TEST_CHECK(std::ranges::all_of(levels, [](float level) { return level == 0.0F; }));
    TEST_CHECK(std::ranges::all_of(caps, [](float cap) { return cap == 0.0F; }));
    TEST_NEAR(bars.backdrop(), 0.25F, 1e-4F);
  }

  void hostileFramesStayInRange() {
    BarScene bars;
    AudioFrameSource source(AudioScene::Hostile);
    std::array<float, 64> levels{};
    std::array<float, 64> caps{};
    for (int frame = 0; frame < 40; ++frame) {
      bars.advance(source.next());
      bars.bars(levels, caps);
      for (std::size_t i = 0; i < levels.size(); ++i) {
        TEST_CHECK(levels[i] >= 0.0F && levels[i] <= 1.0F && caps[i] >= levels[i] && caps[i] <= 1.0F);
      }
      TEST_CHECK(std::ranges::all_of(bars.wave(), [](float y) { return y >= -1.0F && y <= 1.0F; }));
      TEST_CHECK(bars.backdrop() >= 0.0F && bars.backdrop() <= 1.0F);
    }
    // An empty request or one past the band count is defined.
    bars.bars({}, {});
    std::array<float, 80> many{};
    std::array<float, 80> manyCaps{};
    bars.bars(many, manyCaps);
    TEST_CHECK(std::ranges::all_of(many, [](float level) { return level >= 0.0F && level <= 1.0F; }));
  }

  void resetForgetsEverything() {
    BarScene bars = played(AudioScene::Noise, 30);
    bars.reset();
    std::array<float, 64> levels{};
    std::array<float, 64> caps{};
    bars.bars(levels, caps);
    TEST_CHECK(std::ranges::all_of(levels, [](float level) { return level == 0.0F; }));
    TEST_CHECK(std::ranges::all_of(caps, [](float cap) { return cap == 0.0F; }));
    TEST_CHECK(std::ranges::all_of(bars.wave(), [](float y) { return y == 0.0F; }));
    TEST_NEAR(bars.backdrop(), 0.0F, 1e-6F);
  }
} // namespace

int main() {
  barsMergeInNarrowBoxes();
  layoutFollowsTheBox();
  aToneLiftsItsBar();
  theWaveShowsTheNewestSamples();
  kicksJumpThenFallWhileCapsHold();
  silenceSettlesCalm();
  hostileFramesStayInRange();
  resetForgetsEverything();
  std::println("bar scene checks passed");
  return 0;
}
