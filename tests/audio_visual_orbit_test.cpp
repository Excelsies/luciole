#include "audio_visual_checks.h"
#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/pillar_ring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <print>
#include <ranges>
#include <string>

namespace {
  using luciole::workbench::drawOrder;
  using luciole::workbench::kPillarCount;
  using luciole::workbench::kPillarRingRadius;
  using luciole::workbench::OrbitPoint;
  using luciole::workbench::OrbitView;
  using luciole::workbench::pillarHeight;
  using luciole::workbench::PillarRing;
  using luciole::workbench::pillarsBehindCore;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;
  using test::audio::AudioVisualTarget;
  using test::audio::Region;

  constexpr AudioVisualTarget kWide{.width = 900, .height = 500, .scale = 1};
  // A pillar box's half width and the reach of its glow beyond the box, in world units, and the core's center.
  constexpr float kHalfWidth = 0.11F;
  constexpr float kGlowReach = 0.35F;
  constexpr OrbitPoint kCoreCenter{0, 1.2F, 0};
  // A pillar whose level is above this has a glow.
  constexpr float kLitLevel = 0.02F;

  struct Spot {
    int x = 0;
    int y = 0;
  };

  // A place in a box of kWide's size in pixels, top row first.
  struct ScreenPoint {
    float x = 0;
    float y = 0;
  };

  ScreenPoint project(const OrbitView& view, const OrbitPoint& p) {
    const auto& m = view.viewProjection;
    const float cx = (m[0] * p[0]) + (m[4] * p[1]) + (m[8] * p[2]) + m[12];
    const float cy = (m[1] * p[0]) + (m[5] * p[1]) + (m[9] * p[2]) + m[13];
    const float cw = (m[3] * p[0]) + (m[7] * p[1]) + (m[11] * p[2]) + m[15];
    return {.x = ((cx / cw) + 1) * 0.5F * kWide.width, .y = (1 - (cy / cw)) * 0.5F * kWide.height};
  }

  // The point `height` up the middle of pillar `index`.
  OrbitPoint pillarPoint(std::size_t index, float height) {
    const float angle = static_cast<float>(index) * 2 * std::numbers::pi_v<float> / kPillarCount;
    return {kPillarRingRadius * std::cos(angle), height, kPillarRingRadius * std::sin(angle)};
  }

  Spot rounded(ScreenPoint point) {
    return {.x = static_cast<int>(std::lround(point.x)), .y = static_cast<int>(std::lround(point.y))};
  }

  // The pixel at `spot`, which lies inside the image.
  test::audio::Pixel pixelAt(const test::GlPixels& pixels, ScreenPoint spot) {
    const Spot at = rounded(spot);
    TEST_CHECK(at.x >= 0 && at.y >= 0 && at.x < pixels.width && at.y < pixels.height);
    return pixels.at(at.x, at.y);
  }

  // Where a point `height` up the middle of pillar `index` shows in a box of kWide's size, top row first.
  Spot spotOf(const OrbitView& view, std::size_t index, float height) {
    return rounded(project(view, pillarPoint(index, height)));
  }

  // How far `spot` lies from the axis of pillar `index`, `height` tall, in units of `reach` on screen at that
  // height: below 1 inside a column `reach` wide on each side of the axis, which reaches `reach - kHalfWidth` below
  // the floor and above the top. Infinite above or below that column.
  float acrossPillar(const OrbitView& view, std::size_t index, float height, ScreenPoint spot, float reach) {
    const float extra = reach - kHalfWidth;
    const ScreenPoint low = project(view, pillarPoint(index, -extra));
    const ScreenPoint high = project(view, pillarPoint(index, height + extra));
    if (spot.y > low.y || spot.y < high.y || low.y - high.y < 0.5F) {
      return std::numeric_limits<float>::infinity();
    }
    const float t = (low.y - spot.y) / (low.y - high.y);
    const float axis = low.x + (t * (high.x - low.x));
    const OrbitPoint at = pillarPoint(index, -extra + (t * (height + (2 * extra))));
    const ScreenPoint center = project(view, at);
    const ScreenPoint side = project(
        view, {at[0] + (view.right[0] * reach), at[1] + (view.right[1] * reach), at[2] + (view.right[2] * reach)}
    );
    return std::abs(spot.x - axis) / std::hypot(side.x - center.x, side.y - center.y);
  }

  // The brightest luma within 2 px of a spot.
  int lumaNear(const test::GlPixels& pixels, Spot spot) {
    int brightest = 0;
    for (int y = spot.y - 2; y <= spot.y + 2; ++y) {
      for (int x = spot.x - 2; x <= spot.x + 2; ++x) {
        brightest = std::max(brightest, test::audio::luma(pixels.at(x, y)));
      }
    }
    return brightest;
  }

  // The state the visual drew from after `frames` of `scene`.
  PillarRing ringAfter(AudioScene scene, int frames) {
    PillarRing ring;
    AudioFrameSource source(scene);
    for (int frame = 0; frame < frames; ++frame) {
      ring.advance(source.next());
    }
    return ring;
  }

  // The camera the visual drew with after `frames` of `scene`.
  OrbitView cameraAfter(AudioScene scene, int frames) {
    return ringAfter(scene, frames).view(static_cast<float>(kWide.width) / kWide.height);
  }

  // The pillars and the core as they stand in one frame, with the painting order from the farthest to the nearest.
  struct Stage {
    PillarRing ring;
    OrbitView view;
    std::array<std::uint8_t, kPillarCount> order{};
    std::array<std::size_t, kPillarCount> rank{}; // each pillar's place in `order`
    std::size_t behind = 0;                       // how many of `order` stand behind the core

    float height(std::size_t index) const { return pillarHeight(ring.levels()[index]); }
    bool lit(std::size_t index) const { return ring.levels()[index] > kLitLevel; }
    // The middle of a resting pillar's side that faces the camera.
    ScreenPoint middle(std::size_t index) const { return project(view, pillarPoint(index, height(index) * 0.5F)); }
  };

  Stage stageAfter(AudioScene scene, int frames) {
    Stage stage;
    stage.ring = ringAfter(scene, frames);
    stage.view = stage.ring.view(static_cast<float>(kWide.width) / kWide.height);
    stage.order = drawOrder(stage.view.eye);
    for (std::size_t rank = 0; rank < kPillarCount; ++rank) {
      stage.rank.at(stage.order.at(rank)) = rank;
    }
    stage.behind = pillarsBehindCore(stage.view.eye);
    return stage;
  }

  // Whether pillar `index` or its glow may show at `spot`: its box widened by half on each side, or its glow.
  bool mayShowAt(const Stage& stage, std::size_t index, ScreenPoint spot) {
    const float reach = stage.lit(index) ? kHalfWidth + kGlowReach : 1.5F * kHalfWidth;
    return acrossPillar(stage.view, index, stage.height(index), spot, reach) < 1;
  }

  // The nearest pillar other than `except` whose box, widened by half on each side, may show at `spot`; kPillarCount
  // when none does.
  std::size_t nearestBoxAt(const Stage& stage, ScreenPoint spot, std::size_t except) {
    const auto front = std::ranges::find_if(stage.order | std::views::reverse, [&](std::uint8_t other) {
      return other != except && acrossPillar(stage.view, other, stage.height(other), spot, 1.5F * kHalfWidth) < 1;
    });
    return front == (stage.order | std::views::reverse).end() ? kPillarCount : *front;
  }

  // Whether `spot` lies well inside the core's disc: within 0.8 of its radius on screen.
  bool insideCore(const Stage& stage, ScreenPoint spot) {
    const ScreenPoint core = project(stage.view, kCoreCenter);
    const float r = stage.ring.coreRadius();
    const auto& up = stage.view.up;
    const ScreenPoint rim =
        project(stage.view, {kCoreCenter[0] + (up[0] * r), kCoreCenter[1] + (up[1] * r), kCoreCenter[2] + (up[2] * r)});
    return std::hypot(spot.x - core.x, spot.y - core.y) < 0.8F * std::hypot(rim.x - core.x, rim.y - core.y);
  }

  // The core is painted over the pillars behind it: where the middle of a resting pillar behind it shows well inside
  // the core's disc, with no other pillar in front and no glow there, the core's light lies on it.
  void theCoreHidesThePillarsBehindIt(const test::GlPixels& pixels, const Stage& stage) {
    int checked = 0;
    int darkest = 255;
    for (std::size_t rank = 0; rank < stage.behind; ++rank) {
      const std::size_t far = stage.order.at(rank);
      const ScreenPoint spot = stage.middle(far);
      if (stage.lit(far) || !insideCore(stage, spot)) {
        continue;
      }
      const bool clear = std::ranges::none_of(stage.order, [&](std::uint8_t other) {
        return other != far
            && (stage.lit(other) || stage.rank.at(other) >= stage.behind)
            && mayShowAt(stage, other, spot);
      });
      if (clear) {
        darkest = std::min(darkest, test::audio::luma(pixelAt(pixels, spot)));
        ++checked;
      }
    }
    std::println("{} pillars behind the core show through it, the darkest at luma {}", checked, darkest);
    TEST_CHECK(checked >= 3);
    // A far pillar painted over the core shows about 50.
    TEST_CHECK(darkest > 75);
  }

  // A near pillar is painted over the farther ones: where the middle of a resting pillar falls well inside a tall
  // pillar in front of it, with no box nearer still there, the tall pillar's face shows.
  void nearPillarsHideFartherOnes(const test::GlPixels& pixels, const Stage& stage) {
    int checked = 0;
    int darkest = 255;
    for (const std::uint8_t far : stage.order) {
      if (stage.lit(far)) {
        continue;
      }
      const ScreenPoint spot = stage.middle(far);
      const std::size_t tall = nearestBoxAt(stage, spot, far);
      if (tall == kPillarCount
          || stage.rank.at(tall) < stage.rank.at(far)
          || stage.ring.levels()[tall] < 0.5F
          || acrossPillar(stage.view, tall, stage.height(tall), spot, kHalfWidth) > 0.8F) {
        continue;
      }
      darkest = std::min(darkest, test::audio::luma(pixelAt(pixels, spot)));
      ++checked;
    }
    std::println("{} resting pillars stand behind a tall nearer one, the darkest showing luma {}", checked, darkest);
    TEST_CHECK(checked >= 3);
    // A resting pillar painted over the tall one shows about 100, the tall one's glow included.
    TEST_CHECK(darkest > 145);
  }

  // The near pillars are painted over the core: along the axis of a tall pillar in front of the core, where it
  // crosses the core's disc with no box nearer still, the pillar's face shows. With the rig's palette the low bands'
  // face is bluish and the core warm, so blue exceeds red there by much more than where the core lies on top.
  void nearPillarsHideTheCore(const test::GlPixels& pixels, const Stage& stage) {
    constexpr int kSamples = 32;
    int checked = 0;
    int leastBlue = 255;
    for (std::size_t rank = stage.behind; rank < kPillarCount; ++rank) {
      const std::size_t near = stage.order.at(rank);
      if (stage.ring.levels()[near] < 0.5F) {
        continue;
      }
      for (int sample = 1; sample < kSamples; ++sample) {
        const float up = stage.height(near) * static_cast<float>(sample) / kSamples;
        const ScreenPoint spot = project(stage.view, pillarPoint(near, up));
        if (!insideCore(stage, spot) || nearestBoxAt(stage, spot, kPillarCount) != near) {
          continue;
        }
        const auto pixel = pixelAt(pixels, spot);
        leastBlue = std::min(leastBlue, pixel[2] - pixel[0]);
        ++checked;
      }
    }
    std::println("{} spots of near pillars cross the core, blue above red by at least {}", checked, leastBlue);
    TEST_CHECK(checked >= 5);
    // The core painted over the pillar leaves blue at most about 30 above red.
    TEST_CHECK(leastBlue > 40);
  }

  // A tone in band 30 raises pillars 15 and 48, one on each side; the camera moves between two frames.
  void aToneRaisesTwoPillars(AudioVisualRig& rig) {
    auto visual = makeOrbitVisual();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, 60, kWide);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("orbit", "tone", kWide));
    const OrbitView view = cameraAfter(AudioScene::Tone, 60);
    for (const std::size_t tall : {15U, 48U}) {
      const int lit = lumaNear(capture.pixels, spotOf(view, tall, 1.5F));
      std::println("pillar {} at 1.5 up: luma {}", tall, lit);
      TEST_CHECK(lit > 120);
    }
    for (const std::size_t quiet : {24U, 40U}) {
      const int dark = lumaNear(capture.pixels, spotOf(view, quiet, 1.5F));
      std::println("empty air over pillar {}: luma {}", quiet, dark);
      TEST_CHECK(dark < 60);
    }
    const auto next = rig.run(*visual, source, 1, kWide);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, next.pixels, 2) > 100);
    visual->release();
  }

  // A kick lifts the front pillars, which carry the lowest bands, and brightens the whole picture; both have
  // settled by the next kick.
  void kicksLiftTheFront(AudioVisualRig& rig) {
    auto visual = makeOrbitVisual();
    AudioFrameSource source(AudioScene::Kicks);
    (void)rig.run(*visual, source, 60, kWide);
    const auto struck = rig.run(*visual, source, 1, kWide);
    const auto later = rig.run(*visual, source, 25, kWide);
    test::audio::keepForViewing(struck.pixels, test::audio::captureName("orbit", "kick", kWide));
    test::audio::keepForViewing(later.pixels, test::audio::captureName("orbit", "after-kick", kWide));
    const OrbitView atKick = cameraAfter(AudioScene::Kicks, 61);
    const OrbitView beforeNext = cameraAfter(AudioScene::Kicks, 86);
    for (const std::size_t low : {1U, 62U}) {
      const int lifted = lumaNear(struck.pixels, spotOf(atKick, low, 0.8F));
      const int fallen = lumaNear(later.pixels, spotOf(beforeNext, low, 0.8F));
      std::println("pillar {} at 0.8 up: luma {} at the kick, {} before the next", low, lifted, fallen);
      TEST_CHECK(lifted > fallen + 40);
    }
    const double brighter =
        test::audio::distanceFromBackground(struck.pixels) / test::audio::distanceFromBackground(later.pixels);
    std::println("the kick frame holds {:.2f} times the light of the frame before the next", brighter);
    TEST_CHECK(brighter > 1.3);
    visual->release();
  }

  // Painting order, far to near with the core between: three frames after the first kick the lifted front pillars
  // stand before the core; at the third kick the far pillars lie under the core and the front pillars over them.
  void nearerThingsCoverFartherOnes(AudioVisualRig& rig) {
    auto visual = makeOrbitVisual();
    AudioFrameSource source(AudioScene::Kicks);
    const auto early = rig.run(*visual, source, 3, kWide);
    test::audio::keepForViewing(early.pixels, test::audio::captureName("orbit", "first-kick", kWide));
    nearPillarsHideTheCore(early.pixels, stageAfter(AudioScene::Kicks, 3));
    const auto struck = rig.run(*visual, source, 58, kWide);
    const Stage stage = stageAfter(AudioScene::Kicks, 61);
    nearPillarsHideFartherOnes(struck.pixels, stage);
    theCoreHidesThePillarsBehindIt(struck.pixels, stage);
    visual->release();
  }

  // Silence: short pillars, the floor ring and the core, drifting slowly with no flash.
  void silenceDriftsCalmly(AudioVisualRig& rig) {
    auto visual = makeOrbitVisual();
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(*visual, source, 120, kWide);
    test::audio::keepForViewing(capture.pixels, test::audio::captureName("orbit", "silence", kWide));
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    const auto whole = test::audio::wholeRegion(capture.pixels);
    TEST_CHECK(test::audio::pixelsOtherThan(capture.pixels, background, 8, whole).count > 20000);
    // The core above the ring's center shows as a bright disc in the middle.
    const Region core{.left = 430, .top = 200, .right = 470, .bottom = 240};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, 120, core).count > 100);
    const auto later = rig.run(*visual, source, 30, kWide);
    const double change =
        test::audio::distanceFromBackground(later.pixels) / test::audio::distanceFromBackground(capture.pixels);
    std::println("half a second of silence changes the light by a factor {:.3f}", change);
    TEST_NEAR(change, 1.0, 0.03);
    TEST_CHECK(test::audio::differingPixels(capture.pixels, later.pixels, 2) > 0);
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
  const test::audio::VisualFactory make = [] { return makeOrbitVisual(); };
  aToneRaisesTwoPillars(*rig);
  kicksLiftTheFront(*rig);
  nearerThingsCoverFartherOnes(*rig);
  silenceDriftsCalmly(*rig);
  test::audio::checkCaptureSizes(*rig, make, "orbit", AudioScene::Noise, "noise", 90);
  test::audio::checkCaptureSizes(*rig, make, "orbit", AudioScene::Kicks, "kicks", 62);
  test::audio::checkHostileAndEmpty(*rig, make);
  test::audio::checkRunsAndObjects(*rig, make);
  std::println("orbit visual checks passed");
  return 0;
}
