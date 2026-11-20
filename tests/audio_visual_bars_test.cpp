#include "audio_visual_checks.h"
#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/bar_scene.h"

#include <cmath>
#include <memory>
#include <print>
#include <string>

namespace {
  using luciole::workbench::BarLayout;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::Region;

  constexpr AudioVisualTarget kTall{.width = 450, .height = 600, .scale = 1};
  // The bar fill is brighter than this a tenth of the way up; the backdrop glow and the background stay darker.
  constexpr int kBarLuma = 80;
  // Caps and the waveform line are brighter than this; nothing else is.
  constexpr int kLineLuma = 150;

  BarLayout tallLayout() { return luciole::workbench::barLayout(450, 600, 1); }

  // Bar `index`'s columns, a px in from each side, between two rows.
  Region barColumn(const BarLayout& layout, int index, float top, float bottom) {
    const float left = layout.margin + (static_cast<float>(index) * layout.slot) + (layout.gap / 2);
    return {
        .left = static_cast<int>(std::ceil(left)) + 1,
        .top = static_cast<int>(std::ceil(top)),
        .right = static_cast<int>(std::floor(left + layout.slot - layout.gap)) - 1,
        .bottom = static_cast<int>(std::floor(bottom)),
    };
  }

  int area(const Region& region) { return (region.right - region.left) * (region.bottom - region.top); }

  double share(const test::GlPixels& pixels, int minLuma, const Region& region) {
    return static_cast<double>(test::audio::brightPixels(pixels, minLuma, region).count) / area(region);
  }

  double meanLuma(const test::GlPixels& pixels, const Region& region) {
    double sum = 0;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        sum += test::audio::luma(pixels.at(x, y));
      }
    }
    return sum / area(region);
  }

  // Rows below the waveform and its glow but within the lower part of the bars.
  Region lowerBars(const BarLayout& layout, int index) {
    const float below = layout.waveCenter + layout.waveAmplitude + layout.glowWidth + 2;
    return barColumn(layout, index, below, layout.base - (0.1F * (layout.base - layout.top)));
  }

  // A steady tone in band 30 lifts bar 15 of 32 to nine tenths with its cap above it; the other bars stay stubs and
  // the waveform line moves between two frames while the bars do not.
  void toneLiftsOneBar(AudioVisualRig& rig) {
    auto visual = makeBarsVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, 60, kTall);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("bars", "tone", kTall));
    const BarLayout layout = tallLayout();
    TEST_CHECK(layout.count == 32);
    TEST_CHECK(share(capture.pixels, kBarLuma, lowerBars(layout, 15)) > 0.99);
    for (const int quiet : {0, 5, 14, 16, 31}) {
      TEST_CHECK(test::audio::brightPixels(capture.pixels, kBarLuma, lowerBars(layout, quiet)).count == 0);
    }
    const float capTop = layout.base - ((0.9F * layout.maxHeight) + (2 * layout.capHeight));
    TEST_CHECK(share(capture.pixels, 200, barColumn(layout, 15, capTop, capTop + layout.capHeight)) > 0.6);
    TEST_CHECK(
        test::audio::brightPixels(capture.pixels, kBarLuma, barColumn(layout, 15, capTop - 12, capTop - 2)).count == 0
    );

    // The line crosses the left part of the box within its amplitude, and nothing that bright lies above it.
    const int waveTop = static_cast<int>(layout.waveCenter - layout.waveAmplitude - layout.glowWidth);
    const int waveBottom = static_cast<int>(layout.waveCenter + layout.waveAmplitude + layout.glowWidth);
    const Region band{.left = 20, .top = waveTop, .right = 180, .bottom = waveBottom};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kLineLuma, band).count > 300);
    const Region above{.left = 0, .top = 0, .right = 180, .bottom = waveTop - 4};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kLineLuma, above).count == 0);

    const auto next = rig.run(*visual, source, 1, kTall);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, next.pixels, 0) > 200);
    const Region bars{.left = 0, .top = waveBottom + 2, .right = 450, .bottom = 600};
    int changedBelow = 0;
    for (int y = bars.top; y < bars.bottom; ++y) {
      for (int x = bars.left; x < bars.right; ++x) {
        const auto p = capture.pixels.at(x, y);
        const auto q = next.pixels.at(x, y);
        changedBelow += std::abs(p[0] - q[0]) > 1 || std::abs(p[1] - q[1]) > 1 || std::abs(p[2] - q[2]) > 1 ? 1 : 0;
      }
    }
    TEST_CHECK(changedBelow == 0);
    visual->release();
  }

  // A kick lifts the lowest bar to the top and brightens the glow at the bottom; by the next kick the bar has
  // fallen while its cap still holds near the top.
  void kicksJumpAndCapsHold(AudioVisualRig& rig) {
    auto visual = makeBarsVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const auto struck = rig.run(*visual, source, 1, kTall);
    const auto later = rig.run(*visual, source, 25, kTall);
    test::audio::keepForViewing(struck.pixels, test::audio::captureName("bars", "kick", kTall));
    test::audio::keepForViewing(later.pixels, test::audio::captureName("bars", "after-kick", kTall));
    const BarLayout layout = tallLayout();
    TEST_CHECK(share(struck.pixels, kBarLuma, lowerBars(layout, 0)) > 0.99);
    TEST_CHECK(test::audio::brightPixels(later.pixels, kBarLuma, lowerBars(layout, 0)).count == 0);
    const float capTop = layout.base - (layout.maxHeight + (2 * layout.capHeight));
    TEST_CHECK(share(later.pixels, 200, barColumn(layout, 0, capTop, capTop + layout.capHeight)) > 0.6);
    // Bars far from the kick stay stubs.
    TEST_CHECK(test::audio::brightPixels(struck.pixels, kBarLuma, lowerBars(layout, 20)).count == 0);
    const Region glow{.left = 185, .top = 550, .right = 265, .bottom = 600};
    const double brighter = meanLuma(struck.pixels, glow) - meanLuma(later.pixels, glow);
    std::println("the kick brightens the bottom glow by {:.1f} luma", brighter);
    TEST_CHECK(brighter > 5);
    visual->release();
  }

  // Silence: a stub and a cap over every bar, a flat line through the middle, and the same picture frame after
  // frame.
  void silenceIsCalm(AudioVisualRig& rig) {
    auto visual = makeBarsVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, 120, kTall);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("bars", "silence", kTall));
    const BarLayout layout = tallLayout();
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    for (int bar = 0; bar < static_cast<int>(layout.count); ++bar) {
      const Region stub = barColumn(layout, bar, layout.base - layout.capHeight, layout.base);
      TEST_CHECK(test::audio::pixelsOtherThan(capture.pixels, background, 20, stub).count >= area(stub) * 9 / 10);
      const float capTop = layout.base - (2 * layout.capHeight);
      TEST_CHECK(share(capture.pixels, 200, barColumn(layout, bar, capTop, capTop + layout.capHeight)) > 0.6);
    }
    const auto center = static_cast<int>(layout.waveCenter);
    const Region line{.left = 0, .top = center - 2, .right = 450, .bottom = center + 3};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kLineLuma, line).count >= 450);
    const Region above{.left = 0, .top = 0, .right = 450, .bottom = center - 8};
    const auto capRow = static_cast<int>(layout.base - (2 * layout.capHeight));
    const Region below{.left = 0, .top = center + 8, .right = 450, .bottom = capRow - 2};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kBarLuma, above).count == 0);
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kBarLuma, below).count == 0);
    const auto next = rig.run(*visual, source, 1, kTall);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, next.pixels, 0) == 0);
    visual->release();
  }

  // Noise keeps every bar at least a quarter high.
  void noiseLiftsEveryBar(AudioVisualRig& rig) {
    auto visual = makeBarsVisual();
    AudioFrameSource source(AudioScene::Noise);
    const auto capture = rig.run(*visual, source, 60, kTall);
    const BarLayout layout = tallLayout();
    for (int bar = 0; bar < static_cast<int>(layout.count); ++bar) {
      const Region low = barColumn(layout, bar, layout.base - (0.2F * layout.maxHeight), layout.base - 20);
      TEST_CHECK(share(capture.pixels, kBarLuma, low) > 0.9);
    }
    visual->release();
  }

  // A wide box shows 64 bars: the tone's bar is half as wide as at 32.
  void wideBoxesShowMoreBars(AudioVisualRig& rig) {
    auto visual = makeBarsVisual();
    AudioFrameSource source(AudioScene::Tone);
    const AudioVisualTarget wide{.width = 900, .height = 500, .scale = 1};
    const auto capture = rig.run(*visual, source, 60, wide);
    const BarLayout layout = luciole::workbench::barLayout(900, 500, 1);
    TEST_CHECK(layout.count == 64);
    TEST_CHECK(share(capture.pixels, kBarLuma, lowerBars(layout, 30)) > 0.99);
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kBarLuma, lowerBars(layout, 31)).count == 0);
    TEST_CHECK(test::audio::brightPixels(capture.pixels, kBarLuma, lowerBars(layout, 29)).count == 0);
    visual->release();
  }
} // namespace

int main() {
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  const test::audio::VisualFactory make = [] { return makeBarsVisual(); };
  toneLiftsOneBar(*rig);
  kicksJumpAndCapsHold(*rig);
  silenceIsCalm(*rig);
  noiseLiftsEveryBar(*rig);
  wideBoxesShowMoreBars(*rig);
  test::audio::checkCaptureSizes(*rig, make, "bars", AudioScene::Noise, "noise", 90);
  test::audio::checkCaptureSizes(*rig, make, "bars", AudioScene::Kicks, "kicks", 62);
  test::audio::checkHostileAndEmpty(*rig, make);
  test::audio::checkRunsAndObjects(*rig, make);
  std::println("bars visual checks passed");
  return 0;
}
