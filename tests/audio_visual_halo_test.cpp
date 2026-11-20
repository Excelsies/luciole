#include "audio_visual_checks.h"
#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <print>
#include <string>

namespace {
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::Region;

  constexpr AudioVisualTarget kTall{.width = 450, .height = 600, .scale = 1};
  // The shorter side of kTall.
  constexpr double kSide = 450;
  // Band 30 of 64 lies this many degrees from the right, above and below the center.
  const double kToneDegrees = 180.0 * (1.0 - (30.0 / 63.0));

  // The distance from the box's center, in px, of the brightest pixel along a ray `degrees` counterclockwise from
  // the right, looking from `from` to `to` px out.
  double brightestAlong(const test::GlPixels& pixels, double degrees, double from, double to) {
    const double angle = degrees * std::numbers::pi / 180.0;
    const double cx = pixels.width / 2.0;
    const double cy = pixels.height / 2.0;
    double best = from;
    int brightest = -1;
    for (double r = from; r <= to; r += 0.25) {
      const auto x = static_cast<int>(std::floor(cx + (r * std::cos(angle))));
      const auto y = static_cast<int>(std::floor(cy - (r * std::sin(angle))));
      const int value = test::audio::luma(pixels.at(x, y));
      if (value > brightest) {
        brightest = value;
        best = r;
      }
    }
    return best;
  }

  double meanLuma(const test::GlPixels& pixels, const Region& region) {
    double sum = 0;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        sum += test::audio::luma(pixels.at(x, y));
      }
    }
    return sum / ((region.right - region.left) * (region.bottom - region.top));
  }

  const Region kCore{.left = 205, .top = 280, .right = 245, .bottom = 320};

  // Silence: a round ring a fifth of the shorter side out, the same in every direction and in every frame.
  void silenceRestsOnARoundRing(AudioVisualRig& rig) {
    auto visual = makeHaloVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, 120, kTall);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("halo", "silence", kTall));
    for (const double degrees : {0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0}) {
      TEST_NEAR(brightestAlong(capture.pixels, degrees, 40, 200), 0.2 * kSide, 2.0);
    }
    TEST_CHECK(test::audio::brightPixels(capture.pixels, 200, test::audio::wholeRegion(capture.pixels)).count > 400);
    const auto next = rig.run(*visual, source, 1, kTall);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, next.pixels, 0) == 0);
    visual->release();
  }

  // A tone in band 30, softened to 0.45 in the band row, pushes the ring out just left of straight up and down, to
  // 0.24 + 0.12 * 0.45 of the side, while the right side, the highest band, stays at 0.24; its ripple turns between
  // two frames.
  void aToneBulgesItsBand(AudioVisualRig& rig) {
    auto visual = makeHaloVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, 120, kTall);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("halo", "tone", kTall));
    // The ripple moves the ring by up to 0.0051 of the side.
    const double ripple = (0.0051 * kSide) + 2;
    TEST_NEAR(brightestAlong(capture.pixels, kToneDegrees, 40, 220), 0.294 * kSide, ripple);
    TEST_NEAR(brightestAlong(capture.pixels, -kToneDegrees, 40, 220), 0.294 * kSide, ripple);
    TEST_NEAR(brightestAlong(capture.pixels, 0, 40, 220), 0.24 * kSide, ripple);
    TEST_NEAR(brightestAlong(capture.pixels, 180, 40, 220), 0.24 * kSide, ripple);
    TEST_NEAR(brightestAlong(capture.pixels, 45, 40, 220), 0.24 * kSide, ripple);
    const auto next = rig.run(*visual, source, 1, kTall);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, next.pixels, 2) > 100);
    // The corners stay dark.
    const Region corner{.left = 0, .top = 0, .right = 60, .bottom = 60};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, 60, corner).count == 0);
    visual->release();
  }

  // A kick flashes the core and pushes the left side, where the low bands are, far out; both settle before the next.
  void kicksFlashTheCore(AudioVisualRig& rig) {
    auto visual = makeHaloVisual();
    AudioFrameSource source(AudioScene::Kicks);
    (void)rig.run(*visual, source, 30, kTall);
    const auto struck = rig.run(*visual, source, 1, kTall);
    const auto later = rig.run(*visual, source, 25, kTall);
    test::audio::keepForViewing(struck.pixels, test::audio::captureName("halo", "kick", kTall));
    test::audio::keepForViewing(later.pixels, test::audio::captureName("halo", "after-kick", kTall));
    const double flash = meanLuma(struck.pixels, kCore) - meanLuma(later.pixels, kCore);
    std::println("a kick brightens the core by {:.1f} luma", flash);
    TEST_CHECK(flash > 20);
    const double pushed = brightestAlong(struck.pixels, 180, 40, 220);
    const double settled = brightestAlong(later.pixels, 180, 40, 220);
    std::println("the low side lies {:.1f} px out at a kick and {:.1f} px before the next", pushed, settled);
    TEST_CHECK(pushed > settled + (0.08 * kSide));
    visual->release();
  }

  // In a wide box the ring keeps its proportions to the shorter side and stays round.
  void wideBoxesKeepTheRingRound(AudioVisualRig& rig) {
    auto visual = makeHaloVisual();
    AudioFrameSource source(AudioScene::Silence);
    const AudioVisualTarget wide{.width = 900, .height = 500, .scale = 1};
    const auto capture = rig.run(*visual, source, 60, wide);
    for (const double degrees : {0.0, 90.0, 180.0, 270.0}) {
      TEST_NEAR(brightestAlong(capture.pixels, degrees, 40, 240), 0.2 * 500, 2.0);
    }
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
  const test::audio::VisualFactory make = [] { return makeHaloVisual(); };
  silenceRestsOnARoundRing(*rig);
  aToneBulgesItsBand(*rig);
  kicksFlashTheCore(*rig);
  wideBoxesKeepTheRingRound(*rig);
  test::audio::checkCaptureSizes(*rig, make, "halo", AudioScene::Noise, "noise", 90);
  test::audio::checkCaptureSizes(*rig, make, "halo", AudioScene::Kicks, "kicks", 62);
  test::audio::checkHostileAndEmpty(*rig, make);
  test::audio::checkRunsAndObjects(*rig, make);
  std::println("halo visual checks passed");
  return 0;
}
