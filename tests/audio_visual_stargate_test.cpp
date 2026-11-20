#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_history.h"
#include "workbench/stargate_motion.h"
#include "workbench/stargate_rows.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualCapture;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;

  // The roll after `frames` frames of `scene`, from the same motion the visual runs.
  float rollAfter(AudioScene scene, int frames) {
    luciole::workbench::StargateFlight flight;
    luciole::workbench::AudioHistory history(luciole::workbench::kStargateLanes, luciole::workbench::kStargateRows);
    AudioFrameSource source(scene);
    for (int i = 0; i < frames; ++i) {
      flight.advance(source.next(), history);
    }
    return flight.roll();
  }

  // A pixel's place against the rolled horizon, in buffer pixels: `across` toward the upper plane, `along` the
  // horizon.
  struct Place {
    double across = 0;
    double along = 0;
  };

  Place placeOf(const test::GlPixels& image, int x, int y, float roll) {
    const double px = x + 0.5 - (image.width / 2.0);
    const double py = (image.height / 2.0) - (y + 0.5);
    return {
        .across = (std::cos(roll) * py) - (std::sin(roll) * px), .along = (std::cos(roll) * px) + (std::sin(roll) * py)
    };
  }

  struct BandStats {
    int pixels = 0;
    int bright = 0;
    double meanLuma = 0;
  };

  // Pixels whose distance across the horizon is within [from, to].
  BandStats band(const test::GlPixels& image, float roll, double from, double to, int minLuma) {
    BandStats stats;
    double sum = 0;
    for (int y = 0; y < image.height; ++y) {
      for (int x = 0; x < image.width; ++x) {
        const auto place = placeOf(image, x, y, roll);
        if (place.across < from || place.across > to) {
          continue;
        }
        const int luma = test::audio::luma(image.at(x, y));
        ++stats.pixels;
        sum += luma;
        stats.bright += luma >= minLuma ? 1 : 0;
      }
    }
    stats.meanLuma = stats.pixels > 0 ? sum / stats.pixels : 0;
    return stats;
  }

  // Mean luma for each whole pixel of distance below the horizon, from 0 to `depth`, near the middle of the view.
  std::vector<double> lowerProfile(const test::GlPixels& image, float roll, int depth, double halfWidth) {
    std::vector<double> sums(static_cast<std::size_t>(depth), 0.0);
    std::vector<int> counts(static_cast<std::size_t>(depth), 0);
    for (int y = 0; y < image.height; ++y) {
      for (int x = 0; x < image.width; ++x) {
        const auto place = placeOf(image, x, y, roll);
        const auto below = static_cast<int>(std::floor(-place.across));
        if (below < 0 || below >= depth || std::abs(place.along) > halfWidth) {
          continue;
        }
        sums[static_cast<std::size_t>(below)] += test::audio::luma(image.at(x, y));
        ++counts[static_cast<std::size_t>(below)];
      }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
      sums[i] = counts[i] > 0 ? sums[i] / counts[i] : 0;
    }
    return sums;
  }

  // The outward shift, in pixels from -limit to limit, that best lays `before` over `after` between `from` and `to`
  // pixels below the horizon.
  int outwardShift(const std::vector<double>& before, const std::vector<double>& after, int from, int to, int limit) {
    int best = 0;
    double bestError = std::numeric_limits<double>::infinity();
    for (int shift = -limit; shift <= limit; ++shift) {
      double error = 0;
      for (int d = from; d < to; ++d) {
        const double gap = after[static_cast<std::size_t>(d)] - before[static_cast<std::size_t>(d - shift)];
        error += gap * gap;
      }
      if (error < bestError) {
        bestError = error;
        best = shift;
      }
    }
    return best;
  }

  // The mean of |4 L - the four neighbors' L| over pixels between `from` and `to` pixels from the horizon on either
  // side: how much the picture changes from one pixel to the next there.
  double roughness(const test::GlPixels& image, float roll, double from, double to) {
    double sum = 0;
    int count = 0;
    const auto lumaAt = [&](int x, int y) { return test::audio::luma(image.at(x, y)); };
    for (int y = 1; y + 1 < image.height; ++y) {
      for (int x = 1; x + 1 < image.width; ++x) {
        const double across = std::abs(placeOf(image, x, y, roll).across);
        if (across < from || across > to) {
          continue;
        }
        sum += std::abs((4 * lumaAt(x, y)) - lumaAt(x - 1, y) - lumaAt(x + 1, y) - lumaAt(x, y - 1) - lumaAt(x, y + 1));
        ++count;
      }
    }
    return count > 0 ? sum / count : 0;
  }

  void checkSound(const AudioVisualCapture& capture) {
    TEST_CHECK(capture.stateKept);
    TEST_CHECK(capture.insideOnly);
    TEST_CHECK(capture.meanCpuMilliseconds < 0.5);
  }

  // A steady tone fills both planes with lanes, one above the rolled horizon and one below it, and the horizon glows.
  void toneFillsBothPlanes(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Tone);
    constexpr int kFrames = 180;
    const auto capture = rig.run(*visual, source, kFrames, {.width = 900, .height = 500, .scale = 1});
    checkSound(capture);
    const float roll = rollAfter(AudioScene::Tone, kFrames);
    const double half = capture.pixels.height / 2.0;
    const auto upper = band(capture.pixels, roll, 0.15 * half, 2 * half, 90);
    const auto lower = band(capture.pixels, roll, -2 * half, -0.15 * half, 90);
    const auto horizon = band(capture.pixels, roll, -1.5, 1.5, 90);
    const int background = test::audio::luma(test::audio::pixelOf(test::audio::rigPalette().background));
    std::println(
        "stargate tone, rolled {:.3f}: {:.3f} of the upper plane lit, {:.3f} of the lower, horizon luma {:.1f}", roll,
        static_cast<double>(upper.bright) / upper.pixels, static_cast<double>(lower.bright) / lower.pixels,
        horizon.meanLuma
    );
    TEST_NEAR(roll, 0.75F, 0.01F); // three seconds at a quarter radian a second
    TEST_CHECK(upper.bright > upper.pixels / 25);
    TEST_CHECK(lower.bright > lower.pixels / 25);
    TEST_CHECK(horizon.meanLuma > background + 15);
  }

  // A lane is 41 / d texels wide per buffer pixel at d pixels from the horizon, whatever the size. Where that is
  // above one, 26 to 40 pixels out, lanes too fine for the pixels give way to an even light instead of moiré: the
  // picture there is smoother than nearer the eye, where the lanes are wide enough to draw.
  void farLanesDoNotAlias(AudioVisualRig& rig) {
    for (const auto& size :
         {AudioVisualTarget{.width = 900, .height = 500, .scale = 1},
          AudioVisualTarget{.width = 450, .height = 600, .scale = 1.5F}}) {
      for (const AudioScene scene : {AudioScene::Tone, AudioScene::Noise}) {
        auto visual = makeStargateVisual();
        AudioFrameSource source(scene);
        constexpr int kFrames = 360;
        const auto capture = rig.run(*visual, source, kFrames, size);
        const float roll = rollAfter(scene, kFrames);
        const double far = roughness(capture.pixels, roll, 26, 40);
        const double near = roughness(capture.pixels, roll, 80, 120);
        std::println(
            "stargate {}x{} at {}: roughness {:.2f} where lanes are finer than pixels, {:.2f} nearer", size.width,
            size.height, size.scale, far, near
        );
        TEST_CHECK(far < near / 4);
        visual->release();
      }
    }
  }

  // The lines stream toward the eye: three frames later the pattern below the horizon lies further out.
  void kicksRushTowardTheEye(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const AudioVisualTarget size{.width = 900, .height = 500, .scale = 1};
    // Five seconds fill the planes: a row takes 3.6 s from the horizon to the eye at the base speed.
    const auto before = rig.run(*visual, source, 300, size);
    const auto after = rig.run(*visual, source, 3, size);
    checkSound(before);
    checkSound(after);
    const int half = before.pixels.height / 2;
    const auto first = lowerProfile(before.pixels, rollAfter(AudioScene::Kicks, 300), half, 0.3 * half);
    const auto second = lowerProfile(after.pixels, rollAfter(AudioScene::Kicks, 303), half, 0.3 * half);
    const int shift = outwardShift(first, second, half * 35 / 100, half * 70 / 100, 60);
    std::println("stargate kicks: the lower plane moved {} pixels outward in three frames", shift);
    test::audio::keepForViewing(before.pixels, test::audio::captureName("stargate", "rush-before", size));
    test::audio::keepForViewing(after.pixels, test::audio::captureName("stargate", "rush-after", size));
    TEST_CHECK(shift >= 4 && shift <= 45);
  }

  // A hit brightens the whole picture for a moment; the horizon, where the lines barely move between two frames,
  // shows it plainly.
  void hitFlaresThePicture(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1.5F};
    const auto before = rig.run(*visual, source, 60, size);
    const auto hit = rig.run(*visual, source, 1, size);
    checkSound(hit);
    const auto horizonBefore = band(before.pixels, rollAfter(AudioScene::Kicks, 60), -3, 3, 0);
    const auto horizonHit = band(hit.pixels, rollAfter(AudioScene::Kicks, 61), -3, 3, 0);
    const auto whole = test::audio::wholeRegion(hit.pixels);
    const int brightBefore = test::audio::brightPixels(before.pixels, 100, whole).count;
    const int brightHit = test::audio::brightPixels(hit.pixels, 100, whole).count;
    std::println(
        "stargate hit: horizon luma {:.1f}, the frame before {:.1f}; {} bright pixels, before {}", horizonHit.meanLuma,
        horizonBefore.meanLuma, brightHit, brightBefore
    );
    TEST_CHECK(horizonHit.meanLuma > horizonBefore.meanLuma + 8);
    TEST_CHECK(brightHit > brightBefore);
    test::audio::keepForViewing(hit.pixels, test::audio::captureName("stargate", "hit", size));
  }

  // Silence travels on through dark planes toward a calmly glowing horizon.
  void silenceIsCalm(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Silence);
    constexpr int kFrames = 180;
    const auto capture = rig.run(*visual, source, kFrames, {.width = 900, .height = 500, .scale = 1.5F});
    checkSound(capture);
    const float roll = rollAfter(AudioScene::Silence, kFrames);
    const auto whole = test::audio::wholeRegion(capture.pixels);
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    const auto drawn = test::audio::pixelsOtherThan(capture.pixels, background, 4, whole);
    const auto horizon = band(capture.pixels, roll, -1.5, 1.5, 0);
    const double area = static_cast<double>(capture.pixels.width) * capture.pixels.height;
    std::println(
        "stargate silence: {:.3f} of the box drawn, horizon luma {:.1f}", drawn.count / area, horizon.meanLuma
    );
    TEST_CHECK(test::audio::brightPixels(capture.pixels, 120, whole).count == 0);
    TEST_CHECK(drawn.count > area * 0.01);
    TEST_CHECK(horizon.meanLuma > test::audio::luma(background) + 15);
  }

  void hostileFramesKeepTheContract(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Hostile);
    const auto capture = rig.run(*visual, source, 200, {.width = 450, .height = 600, .scale = 1});
    checkSound(capture);
  }

  void sameFramesSamePixels(AudioVisualRig& rig) {
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1.5F};
    auto first = makeStargateVisual();
    auto second = makeStargateVisual();
    AudioFrameSource firstSource(AudioScene::Noise);
    AudioFrameSource secondSource(AudioScene::Noise);
    const auto a = rig.run(*first, firstSource, 90, size);
    const auto b = rig.run(*second, secondSource, 90, size);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);
  }

  // After release the next draw rebuilds the texture from memory; a reset starts a new flight.
  void releaseRebuildsFromMemory(AudioVisualRig& rig) {
    const AudioVisualTarget size{.width = 900, .height = 500, .scale = 1};
    auto kept = makeStargateVisual();
    auto released = makeStargateVisual();
    AudioFrameSource keptSource(AudioScene::Kicks);
    AudioFrameSource releasedSource(AudioScene::Kicks);
    (void)rig.run(*kept, keptSource, 100, size);
    (void)rig.run(*released, releasedSource, 100, size);
    released->release();
    const auto a = rig.run(*kept, keptSource, 1, size);
    const auto b = rig.run(*released, releasedSource, 1, size);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);
    released->reset();
    auto fresh = makeStargateVisual();
    AudioFrameSource resetSource(AudioScene::Tone);
    AudioFrameSource freshSource(AudioScene::Tone);
    const auto c = rig.run(*released, resetSource, 30, size);
    const auto d = rig.run(*fresh, freshSource, 30, size);
    TEST_CHECK(test::audio::differingPixels(c.pixels, d.pixels, 0) == 0);
  }

  // An opacity that is not a number draws nothing.
  void brokenOpacityDrawsNothing(AudioVisualRig& rig) {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Noise);
    const auto capture = rig.run(*visual, source, 30, {.width = 900, .height = 500, .scale = 1, .opacity = kNan});
    checkSound(capture);
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    TEST_CHECK(
        test::audio::pixelsOtherThan(capture.pixels, background, 0, test::audio::wholeRegion(capture.pixels)).count == 0
    );
    visual->release();
  }

  void emptyBoxDrawsNothing(AudioVisualRig& rig) {
    auto visual = makeStargateVisual();
    AudioFrameSource source(AudioScene::Tone);
    for (const auto& size :
         {AudioVisualTarget{.width = 0, .height = 0, .scale = 1},
          AudioVisualTarget{.width = 0, .height = 300, .scale = 1.5F}}) {
      const auto capture = rig.run(*visual, source, 3, size);
      TEST_CHECK(capture.pixels.rgba.empty());
      TEST_CHECK(capture.insideOnly && capture.stateKept);
    }
  }

  // Every scene at every capture size, kept for viewing when LUCIOLE_CAPTURE_DIR is set.
  void capturesEverySize(AudioVisualRig& rig) {
    constexpr std::array<std::pair<AudioScene, std::string_view>, 5> kScenes{{
        {AudioScene::Tone, "tone"},
        {AudioScene::Noise, "noise"},
        {AudioScene::Kicks, "kicks"},
        {AudioScene::Silence, "silence"},
        {AudioScene::Hostile, "hostile"},
    }};
    for (const auto& [scene, name] : kScenes) {
      for (const auto& size : test::audio::kCaptureTargets) {
        auto visual = makeStargateVisual();
        AudioFrameSource source(scene);
        // Six seconds: the rows have come all the way from the horizon to the eye.
        const auto capture = rig.run(*visual, source, 360, size);
        checkSound(capture);
        std::println(
            "stargate {} {}x{} at {}: {:.4f} ms mean, {:.4f} ms max processor time per frame", name, size.width,
            size.height, size.scale, capture.meanCpuMilliseconds, capture.maxCpuMilliseconds
        );
        test::audio::keepForViewing(capture.pixels, test::audio::captureName("stargate", name, size));
        visual->release();
      }
    }
  }
} // namespace

int main() {
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  toneFillsBothPlanes(*rig);
  farLanesDoNotAlias(*rig);
  kicksRushTowardTheEye(*rig);
  hitFlaresThePicture(*rig);
  silenceIsCalm(*rig);
  hostileFramesKeepTheContract(*rig);
  sameFramesSamePixels(*rig);
  releaseRebuildsFromMemory(*rig);
  brokenOpacityDrawsNothing(*rig);
  emptyBoxDrawsNothing(*rig);
  capturesEverySize(*rig);
  std::println("stargate visual checks passed");
  return 0;
}
