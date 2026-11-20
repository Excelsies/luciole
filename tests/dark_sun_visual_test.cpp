#include "audio_visual_rig.h"
#include "disc_visual_checks.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/sun_horizon.h"

#include <cmath>
#include <print>
#include <string>

// The Dark Sun visual drawn from generated frames: a dark sun over a glowing horizon, an inverted range made of the
// spectrum hanging from it, and a beam that falls on each hit.
namespace {
  using luciole::workbench::layoutSun;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::Region;

  // Horizon at y 300, the sun at 225 by 132 with a radius of 38 to 45, the range from x 112 to 338 and a full peak
  // 126 pixels deep.
  const AudioVisualTarget kPortrait{.width = 450, .height = 600, .scale = 1};

  const test::audio::Pixel& background() {
    static const auto pixel = test::audio::pixelOf(test::audio::rigPalette().background);
    return pixel;
  }

  int lit(const test::GlPixels& pixels, Region region) {
    return test::audio::pixelsOtherThan(pixels, background(), 12, region).count;
  }

  // Pixels of `region` within a few steps of the background.
  int unlit(const test::GlPixels& pixels, Region region) {
    const int all = (region.right - region.left) * (region.bottom - region.top);
    return all - test::audio::pixelsOtherThan(pixels, background(), 4, region).count;
  }

  int meanLuma(const test::GlPixels& pixels, Region region) {
    long sum = 0;
    int count = 0;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        sum += test::audio::luma(pixels.at(x, y));
        ++count;
      }
    }
    return count > 0 ? static_cast<int>(sum / count) : 0;
  }

  // The beam's core below the range.
  const Region kBeam{.left = 224, .top = 480, .right = 227, .bottom = 590};

  void checkComposition(const test::GlPixels& pixels) {
    const auto layout = layoutSun(450, 600, 0);
    TEST_NEAR(layout.horizon, 300.0F, 0.01F);
    // The disc's middle is dark: the darker of the background and the text.
    TEST_CHECK(lit(pixels, {.left = 215, .top = 122, .right = 235, .bottom = 142}) == 0);
    // The sky is tinted, more toward the horizon.
    TEST_CHECK(lit(pixels, {.left = 20, .top = 40, .right = 60, .bottom = 80}) == 40 * 40);
    TEST_CHECK(
        meanLuma(pixels, {.left = 20, .top = 260, .right = 60, .bottom = 280})
        > meanLuma(pixels, {.left = 20, .top = 40, .right = 60, .bottom = 80}) + 10
    );
    // The horizon is a bright line.
    TEST_CHECK(
        test::audio::brightPixels(pixels, 150, {.left = 0, .top = 299, .right = 100, .bottom = 301}).count >= 100
    );
    // The ground away from the range and the beam keeps the background.
    TEST_CHECK(lit(pixels, {.left = 0, .top = 420, .right = 100, .bottom = 600}) == 0);
    TEST_CHECK(lit(pixels, {.left = 350, .top = 420, .right = 450, .bottom = 600}) == 0);
  }

  // In silence the sun rests over its horizon, the range keeps its small jagged ridge and the beam is faint.
  void silenceRests(AudioVisualRig& rig) {
    auto visual = makeDarkSunVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, 120, kPortrait);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    checkComposition(capture.pixels);
    const int ridge = lit(capture.pixels, {.left = 125, .top = 302, .right = 325, .bottom = 315});
    std::println("silence: {} ridge pixels, beam luma {}", ridge, meanLuma(capture.pixels, kBeam));
    TEST_CHECK(ridge > 300);
    TEST_CHECK(lit(capture.pixels, {.left = 125, .top = 330, .right = 215, .bottom = 600}) == 0);
    TEST_CHECK(
        test::audio::brightPixels(capture.pixels, 150, {.left = 0, .top = 330, .right = 450, .bottom = 600}).count == 0
    );
    TEST_CHECK(meanLuma(capture.pixels, kBeam) < 120);
    const int restingBeam = lit(capture.pixels, {.left = 200, .top = 320, .right = 250, .bottom = 321});
    std::println("silence: the beam lights {} pixels of row 320", restingBeam);
    TEST_CHECK(restingBeam <= 12);
    visual->release();
  }

  // Band 30 hangs as two thin peaks halfway between the middle and the ends of the range, at x 170 and 280.
  void toneHangsTwoPeaks(AudioVisualRig& rig) {
    auto visual = makeDarkSunVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, 120, kPortrait);
    checkComposition(capture.pixels);
    const int row = 340;
    const int leftPeak = lit(capture.pixels, {.left = 160, .top = row, .right = 182, .bottom = row + 1});
    const int rightPeak = lit(capture.pixels, {.left = 268, .top = row, .right = 290, .bottom = row + 1});
    const int between = lit(capture.pixels, {.left = 186, .top = row, .right = 215, .bottom = row + 1})
        + lit(capture.pixels, {.left = 235, .top = row, .right = 264, .bottom = row + 1});
    std::println("tone peaks at row {}: {} and {} pixels, {} between", row, leftPeak, rightPeak, between);
    TEST_CHECK(leftPeak >= 2 && rightPeak >= 2 && between == 0);
    visual->release();
  }

  // A kick drops a bright, wide beam that is mostly gone half a second later, deepens the middle of the range, and
  // swells the sun.
  void kicksDropTheBeam(AudioVisualRig& rig) {
    auto visual = makeDarkSunVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const auto beforeHit = rig.run(*visual, source, 30, kPortrait);
    const auto atHit = rig.run(*visual, source, 1, kPortrait);
    const auto early = rig.run(*visual, source, 8, kPortrait);
    const auto late = rig.run(*visual, source, 17, kPortrait);
    for (const auto* capture : {&beforeHit, &atHit, &early, &late}) {
      TEST_CHECK(capture->stateKept && capture->insideOnly);
      checkComposition(capture->pixels);
    }
    const int beamBefore = meanLuma(beforeHit.pixels, kBeam);
    const int beamAtHit = meanLuma(atHit.pixels, kBeam);
    const int beamLate = meanLuma(late.pixels, kBeam);
    std::println("beam luma: {} before the hit, {} at it, {} 25 frames later", beamBefore, beamAtHit, beamLate);
    TEST_CHECK(beamAtHit >= 180 && beamBefore < 120 && beamLate < 120);
    const Region beamRow{.left = 150, .top = 560, .right = 300, .bottom = 561};
    TEST_CHECK(lit(atHit.pixels, beamRow) > lit(late.pixels, beamRow) + 6);
    // The kick's bands sit in the middle of the range, which holds its peaks: deep there, and nowhere else.
    // At row 320 the resting ridge is gone and the faint beam alone lights about ten pixels.
    const int middle = lit(late.pixels, {.left = 200, .top = 320, .right = 250, .bottom = 321});
    const int sides = lit(late.pixels, {.left = 120, .top = 320, .right = 190, .bottom = 321})
        + lit(late.pixels, {.left = 260, .top = 320, .right = 330, .bottom = 321});
    std::println("kick range at row 320: {} middle pixels, {} at the sides", middle, sides);
    TEST_CHECK(middle >= 18 && sides == 0);
    // The disc grows: more of the box around the sun keeps its dark color.
    const Region sun{.left = 165, .top = 72, .right = 285, .bottom = 192};
    const int discBefore = unlit(beforeHit.pixels, sun);
    const int discAfter = unlit(early.pixels, sun);
    std::println("dark disc: {} pixels before the hit, {} after", discBefore, discAfter);
    TEST_CHECK(discAfter > discBefore + 100);
    visual->release();
  }

  // A light theme: dark accents and text on a near-white background.
  AudioVisualPalette lightPalette() {
    return {
        .accents = {rgba(0.40F, 0.32F, 0.62F), rgba(0.38F, 0.36F, 0.44F), rgba(0.49F, 0.32F, 0.38F)},
        .background = rgba(0.98F, 0.97F, 0.99F),
        .text = rgba(0.11F, 0.11F, 0.13F),
    };
  }

  // The mean distance in luma of a region's pixels from `reference`.
  double contrast(const test::GlPixels& pixels, int reference, Region region) {
    double sum = 0;
    int count = 0;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        sum += std::abs(test::audio::luma(pixels.at(x, y)) - reference);
        ++count;
      }
    }
    return count > 0 ? sum / count : 0;
  }

  // On a pale background the sun's light covers the ground with its color instead of adding to it, so at a hit the
  // beam's core below the range and the horizon line stand out, and the ground away from them stays clear. Drawn in
  // a context of its own, whose framebuffer shows the light background.
  void lightThemeShowsTheBeam(test::GlTestContext& gl) {
    const auto palette = lightPalette();
    auto visual = makeDarkSunVisual();
    AudioFrameSource source(AudioScene::Kicks);
    for (int frame = 0; frame <= test::audio::kKickFrames; ++frame) {
      visual->advance(source.next());
    }
    glViewport(0, 0, kPortrait.width, kPortrait.height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl.clear(palette.background.r, palette.background.g, palette.background.b, 1);
    const AudioVisualDraw draw{
        .node =
            CustomDrawContext{
                .surfaceWidth = static_cast<float>(kPortrait.width),
                .surfaceHeight = static_cast<float>(kPortrait.height),
                .pixelScaleX = 1,
                .pixelScaleY = 1,
                .transform = Mat3::translation(0, 0),
                .opacity = 1,
                .width = static_cast<float>(kPortrait.width),
                .height = static_cast<float>(kPortrait.height),
            },
        .rect = {.x = 0, .y = 0, .width = kPortrait.width, .height = kPortrait.height},
        .scale = 1,
        .palette = palette,
    };
    visual->draw(draw);
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    const auto pixels = gl.readPixels();
    test::audio::keepForViewing(pixels, "dark-sun-light-hit-450x600-1x.png");
    const int ground = test::audio::luma(test::audio::pixelOf(palette.background));
    const double beam = contrast(pixels, ground, kBeam);
    const double horizon = contrast(pixels, ground, {.left = 0, .top = 299, .right = 100, .bottom = 301});
    const double away = contrast(pixels, ground, {.left = 0, .top = 420, .right = 100, .bottom = 600});
    std::println(
        "light theme at the hit: beam {:.1f}, horizon {:.1f}, ground away {:.1f} from the background", beam, horizon,
        away
    );
    TEST_CHECK(beam > 60 && horizon > 60 && away < 2);
    visual->release();
  }
} // namespace

int main() {
  std::string reason;
  {
    auto gl = test::GlTestContext::create(kPortrait.width, kPortrait.height, false, reason);
    if (!gl) {
      std::println("skipped: {}", reason);
      return test::kSkipExitCode;
    }
    lightThemeShowsTheBeam(*gl);
  }
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  test::audio::disc::checkContract(*rig, makeDarkSunVisual, "dark-sun");
  silenceRests(*rig);
  toneHangsTwoPeaks(*rig);
  kicksDropTheBeam(*rig);
  std::println("dark sun visual checks passed");
  return 0;
}
