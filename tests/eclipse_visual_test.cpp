#include "audio_visual_rig.h"
#include "disc_visual_checks.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/corona_field.h"

#include <cmath>
#include <print>
#include <string>

// The Eclipse visual drawn from generated frames: a dark disc in a bright ring, the spectrum as a corona around the
// ring, the ring swelling with the bass and a flare traveling out on each hit.
namespace {
  using luciole::workbench::CoronaCircle;
  using luciole::workbench::fitCorona;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::Region;

  const AudioVisualTarget kPortrait{.width = 450, .height = 600, .scale = 1};
  const CoronaCircle kCircle = fitCorona(450, 600);

  // The box between two corners given in units of the circle's radius from its center, y down.
  Region box(float left, float top, float right, float bottom) {
    return {
        .left = static_cast<int>(std::lround(kCircle.centerX + (left * kCircle.radius))),
        .top = static_cast<int>(std::lround(kCircle.centerY + (top * kCircle.radius))),
        .right = static_cast<int>(std::lround(kCircle.centerX + (right * kCircle.radius))),
        .bottom = static_cast<int>(std::lround(kCircle.centerY + (bottom * kCircle.radius))),
    };
  }

  // Pixels clearly away from the background, and pixels as bright as the ring's and the corona's light.
  int lit(const test::GlPixels& pixels, Region region) {
    return test::audio::pixelsOtherThan(pixels, test::audio::pixelOf(test::audio::rigPalette().background), 12, region)
        .count;
  }
  int bright(const test::GlPixels& pixels, Region region) {
    return test::audio::brightPixels(pixels, 120, region).count;
  }

  // The luma of the pixel at `x` and `y` units from the circle's center.
  int lumaAt(const test::GlPixels& pixels, float x, float y) {
    return test::audio::luma(pixels.at(
        static_cast<int>(std::lround(kCircle.centerX + (x * kCircle.radius))),
        static_cast<int>(std::lround(kCircle.centerY + (y * kCircle.radius)))
    ));
  }

  // The disc keeps the background and the ring is bright all round.
  void checkDiscAndRing(const test::GlPixels& pixels) {
    TEST_CHECK(lit(pixels, box(-0.28F, -0.28F, 0.28F, 0.28F)) == 0);
    TEST_CHECK(lumaAt(pixels, 0.53F, 0) >= 150);
    TEST_CHECK(lumaAt(pixels, -0.53F, 0) >= 150);
    TEST_CHECK(lumaAt(pixels, 0, 0.53F) >= 150);
    TEST_CHECK(lumaAt(pixels, 0, -0.53F) >= 150);
  }

  // Band 30 sits just below the horizontal: the corona reaches out on the left and the right, not at the top or the
  // bottom. It drifts slowly while the tone holds.
  void toneReachesOutAtTheSides(AudioVisualRig& rig) {
    auto visual = makeEclipseVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto first = rig.run(*visual, source, 120, kPortrait);
    TEST_CHECK(first.stateKept && first.insideOnly);
    test::audio::keepForViewing(first.pixels, "eclipse-tone-checked.png");
    checkDiscAndRing(first.pixels);
    const int right = lit(first.pixels, box(0.74F, -0.25F, 1.0F, 0.25F));
    const int left = lit(first.pixels, box(-1.0F, -0.25F, -0.74F, 0.25F));
    const int top = lit(first.pixels, box(-0.25F, -1.0F, 0.25F, -0.74F));
    const int bottom = lit(first.pixels, box(-0.25F, 0.74F, 0.25F, 1.0F));
    std::println("tone corona: right {}, left {}, top {}, bottom {}", right, left, top, bottom);
    TEST_CHECK(right > 100 && left > 100);
    TEST_CHECK(top == 0 && bottom == 0);
    const auto later = rig.run(*visual, source, 60, kPortrait);
    const int changed = test::audio::differingPixels(first.pixels, later.pixels, 24);
    std::println("tone drift: {} pixels changed in a second", changed);
    TEST_CHECK(changed > 50);
    checkDiscAndRing(later.pixels);
    visual->release();
  }

  // A kick fills the lowest bands, so the corona grows at the bottom; it swells the ring, and sends a flare out that
  // shows above the ring, where no band reaches.
  void kicksSwellAndFlare(AudioVisualRig& rig) {
    auto visual = makeEclipseVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const auto beforeHit = rig.run(*visual, source, 30, kPortrait);
    const auto atHit = rig.run(*visual, source, 1, kPortrait);
    const auto early = rig.run(*visual, source, 8, kPortrait);
    const auto late = rig.run(*visual, source, 12, kPortrait);
    for (const auto* capture : {&beforeHit, &atHit, &early, &late}) {
      TEST_CHECK(capture->stateKept && capture->insideOnly);
      checkDiscAndRing(capture->pixels);
    }
    const int bottom = bright(early.pixels, box(-0.3F, 0.7F, 0.3F, 1.0F));
    // A flare lengthens the corona all round a little, never past 0.85.
    const int top = bright(early.pixels, box(-0.3F, -1.0F, 0.3F, -0.85F));
    std::println("kick corona: bottom {}, top {}", bottom, top);
    TEST_CHECK(bottom > 40 && top == 0);

    // The lit pixels above the ring move up as the flare travels out.
    const Region above = box(-0.3F, -1.0F, 0.3F, -0.66F);
    const auto flareEarly = test::audio::pixelsOtherThan(
        early.pixels, test::audio::pixelOf(test::audio::rigPalette().background), 12, above
    );
    const auto flareLate = test::audio::pixelsOtherThan(
        late.pixels, test::audio::pixelOf(test::audio::rigPalette().background), 12, above
    );
    std::println(
        "flare above the ring: {} pixels around y {:.1f}, then {} around y {:.1f}", flareEarly.count,
        flareEarly.centerY, flareLate.count, flareLate.centerY
    );
    TEST_CHECK(flareEarly.count > 200 && flareLate.count > 200);
    TEST_CHECK(flareLate.centerY < flareEarly.centerY - 8);

    // The ring is wider along the middle row after the hit than just before it.
    const auto row = static_cast<int>(std::lround(kCircle.centerY));
    const Region rightRow{.left = static_cast<int>(kCircle.centerX), .top = row, .right = 450, .bottom = row + 1};
    const int ringBefore = test::audio::brightPixels(beforeHit.pixels, 150, rightRow).count;
    const int ringAfter = test::audio::brightPixels(early.pixels, 150, rightRow).count;
    std::println("ring width on the middle row: {} px before the hit, {} px after", ringBefore, ringAfter);
    TEST_CHECK(ringAfter >= ringBefore + 3);
    visual->release();
  }

  // Silence leaves the disc and its ring with a short calm fringe: nothing reaches far.
  void silenceIsCalm(AudioVisualRig& rig) {
    auto visual = makeEclipseVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, 120, kPortrait);
    checkDiscAndRing(capture.pixels);
    TEST_CHECK(lit(capture.pixels, box(0.74F, -1.0F, 1.0F, 1.0F)) == 0);
    TEST_CHECK(lit(capture.pixels, box(-1.0F, -1.0F, -0.74F, 1.0F)) == 0);
    TEST_CHECK(lit(capture.pixels, box(-1.0F, -1.0F, 1.0F, -0.74F)) == 0);
    TEST_CHECK(lit(capture.pixels, box(-1.0F, 0.74F, 1.0F, 1.0F)) == 0);
    // The ring's area at least: pi (0.62^2 - 0.44^2) of the radius squared, less a fifth for its edges.
    const int ring = test::audio::brightPixels(capture.pixels, 150, test::audio::wholeRegion(capture.pixels)).count;
    std::println("silence: {} bright pixels", ring);
    TEST_CHECK(ring > 21000);
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
  test::audio::disc::checkContract(*rig, makeEclipseVisual, "eclipse");
  toneReachesOutAtTheSides(*rig);
  kicksSwellAndFlare(*rig);
  silenceIsCalm(*rig);
  std::println("eclipse visual checks passed");
  return 0;
}
