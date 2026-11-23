#include "audio_visual_probe.h"
#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/color.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numeric>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Cosmic Peanut through the rig: a round form of rings in the middle, newest at the top and brightest, drifting
// down, swelling on kicks, smooth circles in silence.
namespace {
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::ProbeVisual;

  constexpr int kSeconds = test::audio::kFramesPerSecond;
  constexpr int kLitTolerance = 8;
  constexpr int kBright = 70;

  const test::audio::Pixel kBackground = test::audio::pixelOf(test::audio::rigPalette().background);

  int litPixels(const test::GlPixels& pixels) {
    return test::audio::pixelsOtherThan(pixels, kBackground, kLitTolerance, test::audio::wholeRegion(pixels)).count;
  }

  void checkRun(const test::audio::AudioVisualCapture& capture, std::string_view what) {
    TEST_CHECK(capture.stateKept);
    TEST_CHECK(capture.insideOnly);
    std::println(
        "{}: {:.4f} ms mean, {:.4f} ms max processor time per frame", what, capture.meanCpuMilliseconds,
        capture.maxCpuMilliseconds
    );
    TEST_CHECK(test::audio::withinCpuBudget(capture.meanCpuMilliseconds));
  }

  test::audio::AudioVisualCapture
  run(AudioVisualRig& rig, AudioVisual& visual, AudioScene scene, int frames, const AudioVisualTarget& target) {
    AudioFrameSource source(scene);
    return rig.run(visual, source, frames, target);
  }

  // After eight seconds of a tone the rings make a round form in the middle that fits the rectangle, its top half
  // (the newest rings) brighter than its bottom half.
  void toneMakesARoundFormAtEverySize(AudioVisualRig& rig) {
    for (const auto& target : test::audio::kCaptureTargets) {
      auto visual = makeCosmicPeanutVisual();
      const auto capture = run(rig, *visual, AudioScene::Tone, 8 * kSeconds, target);
      test::audio::keepForViewing(capture.pixels, test::audio::captureName("cosmic-peanut", "tone", target));
      checkRun(capture, test::audio::captureName("cosmic-peanut", "tone", target));
      const auto& pixels = capture.pixels;
      const auto lit =
          test::audio::pixelsOtherThan(pixels, kBackground, kLitTolerance, test::audio::wholeRegion(pixels));
      TEST_CHECK(lit.count > pixels.width * pixels.height / 20);
      TEST_NEAR(lit.centerX, pixels.width / 2.0, pixels.width * 0.03);
      TEST_NEAR(lit.centerY, pixels.height / 2.0, pixels.height * 0.08);
      const auto box = test::audio::brightBox(pixels, kBright);
      const int side = std::min(pixels.width, pixels.height);
      TEST_CHECK(box.width() > side / 2 && box.height() > side / 2);
      TEST_CHECK(box.width() < side && box.height() < side);
      TEST_NEAR(static_cast<double>(box.width()) / box.height(), 1.0, 0.2);
      const int middle = (box.top + box.bottom) / 2;
      const double ground = test::audio::luma(kBackground) * static_cast<double>(pixels.width);
      const double upper =
          test::audio::lumaSum(pixels, {0, box.top, pixels.width, middle}) - (ground * (middle - box.top));
      const double lower =
          test::audio::lumaSum(pixels, {0, middle, pixels.width, box.bottom}) - (ground * (box.bottom - middle));
      std::println("upper {:.0f}, lower {:.0f}", upper, lower);
      TEST_CHECK(upper > lower * 1.15);
      visual->release();
    }
  }

  void capturesEveryScene(AudioVisualRig& rig) {
    for (const auto& [scene, name] :
         {std::pair{AudioScene::Noise, "noise"}, std::pair{AudioScene::Kicks, "kicks"},
          std::pair{AudioScene::Silence, "silence"}}) {
      for (const auto& target : test::audio::kCaptureTargets) {
        auto visual = makeCosmicPeanutVisual();
        const auto capture = run(rig, *visual, scene, 8 * kSeconds, target);
        test::audio::keepForViewing(capture.pixels, test::audio::captureName("cosmic-peanut", name, target));
        checkRun(capture, test::audio::captureName("cosmic-peanut", name, target));
        TEST_CHECK(litPixels(capture.pixels) > 0);
        visual->release();
      }
    }
  }

  // Half a second of noise in a quiet run makes jagged rings that appear at the top and drift down.
  void newRingsStartAtTheTopAndDriftDown(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 400, .height = 400, .scale = 1};
    auto heard = makeCosmicPeanutVisual();
    auto quiet = makeCosmicPeanutVisual();
    (void)run(rig, *heard, AudioScene::Silence, 4 * kSeconds, target);
    (void)run(rig, *heard, AudioScene::Noise, kSeconds / 2, target);
    const auto early = run(rig, *heard, AudioScene::Silence, kSeconds / 2, target);
    const auto earlyQuiet = run(rig, *quiet, AudioScene::Silence, 5 * kSeconds, target);
    const auto late = run(rig, *heard, AudioScene::Silence, 3 * kSeconds, target);
    const auto lateQuiet = run(rig, *quiet, AudioScene::Silence, 3 * kSeconds, target);

    const auto changed = [](const test::GlPixels& a, const test::GlPixels& b) {
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
        "noise rings: {} pixels at y {:.1f}, then {} pixels at y {:.1f}", first.count, first.centerY, second.count,
        second.centerY
    );
    TEST_CHECK(first.count > 200 && second.count > 200);
    TEST_CHECK(first.centerY < 0.4 * target.height);
    TEST_CHECK(second.centerY > first.centerY + (0.2 * target.height));
    heard->release();
    quiet->release();
  }

  // The rings' far side is dimmer than their near side. Half a second of noise in a quiet run leaves a band of
  // jagged rings; after it drifts to the equator, the difference from the quiet run is that band alone. The camera
  // looks down on it, so its near half lies below the band's middle on screen and its far half above.
  void farSideIsDimmer(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 400, .height = 400, .scale = 1};
    auto heard = makeCosmicPeanutVisual();
    auto quiet = makeCosmicPeanutVisual();
    (void)run(rig, *heard, AudioScene::Silence, 4 * kSeconds, target);
    (void)run(rig, *heard, AudioScene::Noise, kSeconds / 2, target);
    const auto band = run(rig, *heard, AudioScene::Silence, (7 * kSeconds) / 2, target);
    const auto calm = run(rig, *quiet, AudioScene::Silence, 8 * kSeconds, target);
    std::vector<double> rows(static_cast<std::size_t>(band.pixels.height), 0.0);
    for (int y = 0; y < band.pixels.height; ++y) {
      for (int x = 0; x < band.pixels.width; ++x) {
        rows[y] += std::abs(test::audio::luma(band.pixels.at(x, y)) - test::audio::luma(calm.pixels.at(x, y)));
      }
    }
    // The band's rows: those where the difference amounts to more than one full pixel.
    const auto first = std::ranges::find_if(rows, [](double light) { return light > 255; });
    const auto last = std::ranges::find_if(rows | std::views::reverse, [](double light) { return light > 255; });
    TEST_CHECK(first != rows.end() && last != rows.rend());
    const auto top = static_cast<std::size_t>(first - rows.begin());
    const std::size_t bottom = rows.size() - 1 - static_cast<std::size_t>(last - rows.rbegin());
    const std::size_t middle = (top + bottom) / 2;
    const double far = std::accumulate(rows.begin() + top, rows.begin() + middle, 0.0);
    const double near = std::accumulate(rows.begin() + middle, rows.begin() + bottom + 1, 0.0);
    std::println("far side: band rows {} to {}, light {:.0f} above the middle, {:.0f} below", top, bottom, far, near);
    TEST_CHECK(bottom - top > static_cast<std::size_t>(target.height / 4));
    TEST_CHECK(near > far * 1.4);
    heard->release();
    quiet->release();
  }

  // A kick swells the whole form a few percent and brightens it, against the same frames with a steady bass.
  void kicksSwellAndBrighten(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 500, .height = 500, .scale = 1};
    auto kicked = makeCosmicPeanutVisual();
    auto steadyInner = makeCosmicPeanutVisual();
    ProbeVisual steady(*steadyInner, {.steadyBass = true});
    // The last frame is the fourth after a hit, where the pulse is near its height.
    const int frames = (8 * test::audio::kKickFrames) + 4;
    const auto a = run(rig, *kicked, AudioScene::Kicks, frames, target);
    const auto b = run(rig, steady, AudioScene::Kicks, frames, target);
    const double center = target.width / 2.0;
    const double swollen = test::audio::brightSpread(a.pixels, kBright, center, center);
    const double plain = test::audio::brightSpread(b.pixels, kBright, center, center);
    const auto whole = test::audio::wholeRegion(a.pixels);
    const double ground = test::audio::luma(kBackground) * static_cast<double>(a.pixels.width * a.pixels.height);
    const double ratio =
        (test::audio::lumaSum(a.pixels, whole) - ground) / (test::audio::lumaSum(b.pixels, whole) - ground);
    std::println("kick: spread {:.2f} against {:.2f}, light {:.3f} times", swollen, plain, ratio);
    TEST_CHECK(swollen > plain * 1.01 && swollen < plain * 1.05);
    TEST_CHECK(ratio > 1.02);
    kicked->release();
    steady.release();
  }

  // Silence leaves smooth circles: the picture is its own mirror image, unlike under noise.
  void silenceIsCalm(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 450, .height = 600, .scale = 1};
    auto visual = makeCosmicPeanutVisual();
    const auto calm = run(rig, *visual, AudioScene::Silence, 8 * kSeconds, target);
    auto noisy = makeCosmicPeanutVisual();
    const auto busy = run(rig, *noisy, AudioScene::Noise, 8 * kSeconds, target);
    const int calmLit = litPixels(calm.pixels);
    const int calmOdd = test::audio::unmirroredPixels(calm.pixels, kBackground, kLitTolerance);
    const int busyOdd = test::audio::unmirroredPixels(busy.pixels, kBackground, kLitTolerance);
    std::println("silence: {} lit, {} unmirrored; noise: {} unmirrored", calmLit, calmOdd, busyOdd);
    TEST_CHECK(calmLit > calm.pixels.width * calm.pixels.height / 20);
    TEST_CHECK(calmOdd < calmLit / 50);
    TEST_CHECK(busyOdd > 5 * calmOdd + 500);
    visual->release();
    noisy->release();
  }

  void hostileFramesDrawSafely(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 300, .height = 300, .scale = 1.5F};
    auto visual = makeCosmicPeanutVisual();
    const auto capture = run(rig, *visual, AudioScene::Hostile, 4 * kSeconds, target);
    checkRun(capture, "cosmic-peanut hostile");
    const int lit = litPixels(capture.pixels);
    TEST_CHECK(lit > 0 && lit < capture.pixels.width * capture.pixels.height / 2);
    visual->release();
  }

  // A rectangle far taller than wide moves the eye far back, and the form still shows, inside the rectangle.
  void narrowRectangleKeepsTheForm(AudioVisualRig& rig) {
    auto visual = makeCosmicPeanutVisual();
    const auto capture = run(rig, *visual, AudioScene::Tone, 2 * kSeconds, {.width = 30, .height = 600, .scale = 1});
    const int lit = litPixels(capture.pixels);
    std::println("narrow: {} lit", lit);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    TEST_CHECK(lit > 40);
    visual->release();
  }

  void emptyRectangleDrawsNothing(AudioVisualRig& rig) {
    auto visual = makeCosmicPeanutVisual();
    for (const auto& target :
         {AudioVisualTarget{.width = 0, .height = 0, .scale = 1},
          AudioVisualTarget{.width = 300, .height = 0, .scale = 1.5F}}) {
      const auto capture = run(rig, *visual, AudioScene::Tone, 30, target);
      TEST_CHECK(capture.pixels.rgba.empty() && capture.stateKept && capture.insideOnly);
    }
    // Nothing was made, so this is a draw from scratch.
    const auto later = run(rig, *visual, AudioScene::Tone, 2, {.width = 200, .height = 200, .scale = 1});
    TEST_CHECK(later.stateKept && later.insideOnly);
    visual->release();
  }

  // Two runs from the same frames give the same pixels; a release in between, a repeated draw without an advance
  // and the host's opacity change nothing else.
  void sameFramesSamePixels(AudioVisualRig& rig) {
    const AudioVisualTarget target{.width = 320, .height = 240, .scale = 1.5F};
    auto first = makeCosmicPeanutVisual();
    auto second = makeCosmicPeanutVisual();
    const auto a = run(rig, *first, AudioScene::Noise, 200, target);
    AudioFrameSource source(AudioScene::Noise);
    (void)rig.run(*second, source, 150, target);
    second->release();
    const auto b = rig.run(*second, source, 50, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);

    auto third = makeCosmicPeanutVisual();
    ProbeVisual held(*third, {.advances = 200});
    const auto c = run(rig, held, AudioScene::Noise, 203, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, c.pixels, 0) == 0);

    second->reset();
    const auto d = run(rig, *second, AudioScene::Noise, 200, target);
    TEST_CHECK(test::audio::differingPixels(a.pixels, d.pixels, 0) == 0);

    auto faded = makeCosmicPeanutVisual();
    const auto e =
        run(rig, *faded, AudioScene::Noise, 200, {.width = 320, .height = 240, .scale = 1.5F, .opacity = 0.5F});
    // Where the full light did not saturate, half the opacity adds half the light.
    int compared = 0;
    int off = 0;
    for (int y = 0; y < a.pixels.height; ++y) {
      for (int x = 0; x < a.pixels.width; ++x) {
        const auto full = a.pixels.at(x, y);
        if (std::ranges::any_of(std::span(full).first(3), [](int c) { return c >= 250; })) {
          continue;
        }
        ++compared;
        const auto half = e.pixels.at(x, y);
        for (std::size_t c = 0; c < 3; ++c) {
          const double expected = kBackground[c] + (0.5 * (full[c] - kBackground[c]));
          off += std::abs(half[c] - expected) > 2 ? 1 : 0;
        }
      }
    }
    TEST_CHECK(compared > a.pixels.width * a.pixels.height / 2);
    TEST_CHECK(off < compared / 200);
    for (auto* visual : {first.get(), second.get(), third.get(), faded.get()}) {
      visual->release();
    }
  }

  int brightestChannel(const test::GlPixels& pixels) {
    int brightest = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        const auto p = pixels.at(x, y);
        brightest = std::max({brightest, static_cast<int>(p[0]), static_cast<int>(p[1]), static_cast<int>(p[2])});
      }
    }
    return brightest;
  }

  // On a light theme the rings cover the background with their colors instead of adding light, so no pixel comes
  // out brighter than the brightest color in play; the same colors as light on a dark theme pile up past it.
  void lightThemeCoversInsteadOfAddingLight(AudioVisualRig& rig) {
    const std::array<Color, 3> accents{rgba(0.55F, 0.35F, 0.1F), rgba(0.2F, 0.3F, 0.6F), rgba(0.15F, 0.45F, 0.3F)};
    const Color ink = rgba(0.16F, 0.16F, 0.24F);
    const AudioVisualPalette light{.accents = accents, .background = rgba(0.95F, 0.95F, 0.95F), .text = ink};
    const AudioVisualPalette dark{.accents = accents, .background = rgba(0.05F, 0.05F, 0.1F), .text = ink};
    const AudioVisualTarget target{.width = 400, .height = 400, .scale = 1};
    auto coverInner = makeCosmicPeanutVisual();
    ProbeVisual cover(*coverInner, {.palette = light});
    const auto covered = run(rig, cover, AudioScene::Noise, 6 * kSeconds, target);
    auto addInner = makeCosmicPeanutVisual();
    ProbeVisual add(*addInner, {.palette = dark});
    const auto added = run(rig, add, AudioScene::Noise, 6 * kSeconds, target);
    TEST_CHECK(covered.stateKept && covered.insideOnly);
    TEST_CHECK(litPixels(covered.pixels) > 1000);
    // The brightest color in play: the third accent's green, or the second's blue lifted by the ink at most 0.21.
    const int bound = static_cast<int>(std::lround(255 * (0.6F + (0.24F * 0.6F))));
    std::println(
        "light theme: brightest {} covered, {} added, bound {}", brightestChannel(covered.pixels),
        brightestChannel(added.pixels), bound
    );
    TEST_CHECK(brightestChannel(covered.pixels) <= bound);
    TEST_CHECK(brightestChannel(added.pixels) > bound + 20);
    cover.release();
    add.release();
  }
} // namespace

int main() {
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  std::println("renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
  toneMakesARoundFormAtEverySize(*rig);
  capturesEveryScene(*rig);
  newRingsStartAtTheTopAndDriftDown(*rig);
  farSideIsDimmer(*rig);
  kicksSwellAndBrighten(*rig);
  silenceIsCalm(*rig);
  hostileFramesDrawSafely(*rig);
  narrowRectangleKeepsTheForm(*rig);
  emptyRectangleDrawsNothing(*rig);
  sameFramesSamePixels(*rig);
  lightThemeCoversInsteadOfAddingLight(*rig);
  std::println("cosmic peanut checks passed");
  return 0;
}
