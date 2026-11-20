#include "audio_visual_probe.h"
#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/ridge_field.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Pulsar through the rig: ridge lines inside a portrait plot in the middle, flat at the sides, raised in the middle
// by the sound, nearer lines hiding farther ones, every line moving up and back.
namespace {
  using luciole::workbench::RidgeLayout;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::ProbeVisual;

  constexpr int kSeconds = test::audio::kFramesPerSecond;
  constexpr int kLine = 60; // the least luma of a line's middle, far lines included

  const test::audio::Pixel kBackground = test::audio::pixelOf(test::audio::rigPalette().background);

  RidgeLayout layoutOf(const test::GlPixels& pixels) {
    return luciole::workbench::ridgeLayout(static_cast<float>(pixels.width), static_cast<float>(pixels.height));
  }

  // The lines met going down column `x`: runs of pixels with luma at least kLine.
  int crossings(const test::GlPixels& pixels, int x) {
    int count = 0;
    bool inside = false;
    for (int y = 0; y < pixels.height; ++y) {
      const bool bright = test::audio::luma(pixels.at(x, y)) >= kLine;
      count += bright && !inside ? 1 : 0;
      inside = bright;
    }
    return count;
  }

  int column(const RidgeLayout& layout, double share) {
    return static_cast<int>(std::lround(layout.left + (share * layout.width)));
  }

  void checkRun(const test::audio::AudioVisualCapture& capture, std::string_view what) {
    TEST_CHECK(capture.stateKept);
    TEST_CHECK(capture.insideOnly);
    std::println(
        "{}: {:.4f} ms mean, {:.4f} ms max processor time per frame", what, capture.meanCpuMilliseconds,
        capture.maxCpuMilliseconds
    );
    TEST_CHECK(capture.meanCpuMilliseconds < 0.5);
  }

  test::audio::AudioVisualCapture
  run(AudioVisualRig& rig, AudioVisual& visual, AudioScene scene, int frames, const AudioVisualTarget& target) {
    AudioFrameSource source(scene);
    return rig.run(visual, source, frames, target);
  }

  // Every scene at every size: lines only inside the plot's columns, about one line per baseline at the flat sides.
  void linesStayInThePlotAtEverySize(AudioVisualRig& rig) {
    for (const auto& [scene, name] :
         {std::pair{AudioScene::Tone, "tone"}, std::pair{AudioScene::Noise, "noise"},
          std::pair{AudioScene::Kicks, "kicks"}, std::pair{AudioScene::Silence, "silence"}}) {
      for (const auto& target : test::audio::kCaptureTargets) {
        auto visual = makePulsarVisual();
        const auto capture = run(rig, *visual, scene, 8 * kSeconds, target);
        const auto title = test::audio::captureName("pulsar", name, target);
        test::audio::keepForViewing(capture.pixels, title);
        checkRun(capture, title);
        const auto& pixels = capture.pixels;
        const auto layout = layoutOf(pixels);
        const int left = static_cast<int>(std::floor(layout.left)) - 2;
        const int right = static_cast<int>(std::ceil(layout.left + layout.width)) + 2;
        TEST_CHECK(test::audio::brightPixels(pixels, kLine, {0, 0, left, pixels.height}).count == 0);
        TEST_CHECK(test::audio::brightPixels(pixels, kLine, {right, 0, pixels.width, pixels.height}).count == 0);
        const int side = crossings(pixels, column(layout, 0.08));
        std::println("{}: {} lines at the side", title, side);
        TEST_CHECK(side >= 74 && side <= 81);
        visual->release();
      }
    }
  }

  // The top row of the `index`-th line met going down column `x`; -1 when the column meets fewer.
  int lineRow(const test::GlPixels& pixels, int x, int index) {
    int count = 0;
    bool inside = false;
    for (int y = 0; y < pixels.height; ++y) {
      const bool bright = test::audio::luma(pixels.at(x, y)) >= kLine;
      if (bright && !inside && count++ == index) {
        return y;
      }
      inside = bright;
    }
    return -1;
  }

  // How far the `index`-th line rises and falls across the plot, in rows; -1 when a column misses it.
  int lineRange(const test::GlPixels& pixels, const RidgeLayout& layout, int index) {
    int low = pixels.height;
    int high = -1;
    for (int k = 0; k <= 16; ++k) {
      const int row = lineRow(pixels, column(layout, 0.02 + (0.06 * k)), index);
      if (row < 0) {
        return -1;
      }
      low = std::min(low, row);
      high = std::max(high, row);
    }
    return high - low;
  }

  // In silence every line is flat: every column across the plot meets every line at the same height; under noise
  // the middle rises.
  void silenceIsCalm(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 450, .height = 600, .scale = 1};
    auto visual = makePulsarVisual();
    const auto calm = run(rig, *visual, AudioScene::Silence, 8 * kSeconds, target);
    auto noisy = makePulsarVisual();
    const auto busy = run(rig, *noisy, AudioScene::Noise, 8 * kSeconds, target);
    const auto layout = layoutOf(calm.pixels);
    int widest = 0;
    for (const int index : {0, 20, 40, 60, 78}) {
      widest = std::max(widest, lineRange(calm.pixels, layout, index));
      TEST_CHECK(lineRange(calm.pixels, layout, index) >= 0);
    }
    const int busyRange = lineRange(busy.pixels, layout, 40);
    std::println("silence: lines rise and fall at most {} rows; under noise line 40 {}", widest, busyRange);
    TEST_CHECK(widest <= 2);
    TEST_CHECK(busyRange < 0 || busyRange > 10);
    visual->release();
    noisy->release();
  }

  // A raised line hides the lines behind it: where noise raises the lines, a column meets well under half as many
  // of them as at the flat side. Lines that crossed each other instead would leave over half of them in sight.
  void nearerLinesHideFartherOnes(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 450, .height = 600, .scale = 1};
    auto noisy = makePulsarVisual();
    const auto noise = run(rig, *noisy, AudioScene::Noise, 8 * kSeconds, target);
    const auto layout = layoutOf(noise.pixels);
    const int side = crossings(noise.pixels, column(layout, 0.08));
    const int middle = crossings(noise.pixels, column(layout, 0.5));
    std::println("noise: {} lines at the side, {} in the middle", side, middle);
    TEST_CHECK(middle < side * 0.45);
    noisy->release();
  }

  // Half a second of noise in a quiet run raises lines that enter at the front, low in the plot, and move up.
  void linesMoveUpAndBack(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 450, .height = 600, .scale = 1};
    auto heard = makePulsarVisual();
    auto quiet = makePulsarVisual();
    (void)run(rig, *heard, AudioScene::Silence, 2 * kSeconds, target);
    (void)run(rig, *heard, AudioScene::Noise, kSeconds / 2, target);
    const auto early = run(rig, *heard, AudioScene::Silence, kSeconds / 2, target);
    const auto earlyQuiet = run(rig, *quiet, AudioScene::Silence, 3 * kSeconds, target);
    const auto late = run(rig, *heard, AudioScene::Silence, 3 * kSeconds, target);
    const auto lateQuiet = run(rig, *quiet, AudioScene::Silence, 3 * kSeconds, target);
    const auto whole = test::audio::wholeRegion(early.pixels);
    const auto changed = [&](const test::GlPixels& a, const test::GlPixels& b) {
      test::audio::PixelStats stats;
      double sumY = 0;
      for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
          if (std::abs(test::audio::luma(a.at(x, y)) - test::audio::luma(b.at(x, y))) > 12) {
            ++stats.count;
            sumY += y;
          }
        }
      }
      stats.centerY = stats.count > 0 ? sumY / stats.count : 0;
      return stats;
    };
    const auto first = changed(early.pixels, earlyQuiet.pixels);
    const auto second = changed(late.pixels, lateQuiet.pixels);
    std::println(
        "noise lines: {} pixels at y {:.1f}, then {} pixels at y {:.1f}", first.count, first.centerY, second.count,
        second.centerY
    );
    TEST_CHECK(first.count > 200 && second.count > 200);
    TEST_CHECK(first.centerY > 0.65 * whole.bottom);
    TEST_CHECK(second.centerY < first.centerY - (0.2 * whole.bottom));
    heard->release();
    quiet->release();
  }

  // A kick raises a spike in the middle of the lines pushed after it, against the same frames without bands.
  void kicksRaiseTheMiddle(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 450, .height = 600, .scale = 1};
    auto kicked = makePulsarVisual();
    auto flatInner = makePulsarVisual();
    ProbeVisual flat(*flatInner, {.quietBands = true});
    const int frames = (4 * test::audio::kKickFrames) + 12;
    const auto a = run(rig, *kicked, AudioScene::Kicks, frames, target);
    const auto b = run(rig, flat, AudioScene::Kicks, frames, target);
    const auto layout = layoutOf(a.pixels);
    int count = 0;
    int nearMiddle = 0;
    double sumX = 0;
    for (int y = 0; y < a.pixels.height; ++y) {
      for (int x = 0; x < a.pixels.width; ++x) {
        if (std::abs(test::audio::luma(a.pixels.at(x, y)) - test::audio::luma(b.pixels.at(x, y))) > 12) {
          ++count;
          sumX += x;
          nearMiddle += std::abs(x - (layout.left + (layout.width / 2))) < 0.1 * layout.width ? 1 : 0;
        }
      }
    }
    std::println("kick: {} pixels changed, {} near the middle, at x {:.1f}", count, nearMiddle, sumX / count);
    TEST_CHECK(count > 300);
    TEST_CHECK(nearMiddle > count * 0.95);
    TEST_NEAR(sumX / count, layout.left + (layout.width / 2), layout.width * 0.03);
    kicked->release();
    flat.release();
  }

  void hostileFramesDrawSafely(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 300, .height = 300, .scale = 1.5F};
    auto visual = makePulsarVisual();
    const auto capture = run(rig, *visual, AudioScene::Hostile, 4 * kSeconds, target);
    checkRun(capture, "pulsar hostile");
    const auto layout = layoutOf(capture.pixels);
    TEST_CHECK(crossings(capture.pixels, column(layout, 0.08)) > 40);
    visual->release();
  }

  void emptyRectangleDrawsNothing(AudioVisualRig& rig) {
    auto visual = makePulsarVisual();
    for (const auto& target :
         {AudioVisualTarget{.width = 0, .height = 0, .scale = 1},
          AudioVisualTarget{.width = 300, .height = 0, .scale = 1.5F}}) {
      const auto capture = run(rig, *visual, AudioScene::Tone, 30, target);
      TEST_CHECK(capture.pixels.rgba.empty() && capture.stateKept && capture.insideOnly);
    }
    const auto later = run(rig, *visual, AudioScene::Tone, 2, {.width = 200, .height = 200, .scale = 1});
    TEST_CHECK(later.stateKept && later.insideOnly);
    visual->release();
  }

  // Two runs from the same frames give the same pixels; a release in between, a repeated draw without an advance
  // and a reset change nothing else, and half the host's opacity brings the lines half way to the background.
  void sameFramesSamePixels(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 320, .height = 240, .scale = 1.5F};
    auto first = makePulsarVisual();
    auto second = makePulsarVisual();
    const auto a = run(rig, *first, AudioScene::Noise, 200, target);
    AudioFrameSource source(AudioScene::Noise);
    (void)rig.run(*second, source, 150, target);
    second->release();
    const auto b = rig.run(*second, source, 50, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);

    auto third = makePulsarVisual();
    ProbeVisual held(*third, {.advances = 200});
    const auto c = run(rig, held, AudioScene::Noise, 203, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, c.pixels, 0) == 0);

    second->reset();
    const auto d = run(rig, *second, AudioScene::Noise, 200, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, d.pixels, 0) == 0);

    auto full = makePulsarVisual();
    auto faded = makePulsarVisual();
    const auto e = run(rig, *full, AudioScene::Silence, 200, target);
    const auto f =
        run(rig, *faded, AudioScene::Silence, 200,
            {.width = target.width, .height = target.height, .scale = target.scale, .opacity = 0.5F});
    int off = 0;
    for (int y = 0; y < e.pixels.height; ++y) {
      for (int x = 0; x < e.pixels.width; ++x) {
        const auto whole = e.pixels.at(x, y);
        const auto half = f.pixels.at(x, y);
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const double expected = kBackground[channel] + (0.5 * (whole[channel] - kBackground[channel]));
          off += std::abs(half[channel] - expected) > 2 ? 1 : 0;
        }
      }
    }
    TEST_CHECK(off < e.pixels.width * e.pixels.height / 500);
    for (auto* visual : {first.get(), second.get(), third.get(), full.get(), faded.get()}) {
      visual->release();
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
  linesStayInThePlotAtEverySize(*rig);
  silenceIsCalm(*rig);
  nearerLinesHideFartherOnes(*rig);
  linesMoveUpAndBack(*rig);
  kicksRaiseTheMiddle(*rig);
  hostileFramesDrawSafely(*rig);
  emptyRectangleDrawsNothing(*rig);
  sameFramesSamePixels(*rig);
  std::println("pulsar checks passed");
  return 0;
}
