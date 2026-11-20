#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/pillar_ring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <print>

namespace {
  using namespace luciole::workbench;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  struct Clip {
    float x = 0;
    float y = 0;
    float w = 0;
  };

  Clip project(const OrbitMatrix& m, float x, float y, float z) {
    const float cx = (m[0] * x) + (m[4] * y) + (m[8] * z) + m[12];
    const float cy = (m[1] * x) + (m[5] * y) + (m[9] * z) + m[13];
    const float cw = (m[3] * x) + (m[7] * y) + (m[11] * z) + m[15];
    return {.x = cx / cw, .y = cy / cw, .w = cw};
  }

  float pillarX(std::size_t index) {
    return kPillarRingRadius * std::cos(static_cast<float>(index) * 2 * std::numbers::pi_v<float> / kPillarCount);
  }

  float pillarZ(std::size_t index) {
    return kPillarRingRadius * std::sin(static_cast<float>(index) * 2 * std::numbers::pi_v<float> / kPillarCount);
  }

  // The camera's angle around the ring, from +x toward +z.
  float eyeAngle(const PillarRing& ring) {
    const OrbitView view = ring.view(1);
    return std::atan2(view.eye[2], view.eye[0]);
  }

  PillarRing played(AudioScene scene, int frames) {
    PillarRing ring;
    AudioFrameSource source(scene);
    for (int frame = 0; frame < frames; ++frame) {
      ring.advance(source.next());
    }
    return ring;
  }

  void bandsFoldOverTheRing() {
    TEST_CHECK(pillarBand(0) == 0);
    TEST_CHECK(pillarBand(1) == 2);
    TEST_CHECK(pillarBand(15) == 30);
    TEST_CHECK(pillarBand(31) == 62);
    TEST_CHECK(pillarBand(32) == 62);
    TEST_CHECK(pillarBand(48) == 30);
    TEST_CHECK(pillarBand(63) == 0);
    TEST_CHECK(pillarBand(500) == 0);
    TEST_NEAR(pillarHeight(0), 0.08F, 1e-6F);
    TEST_NEAR(pillarHeight(1), 3.28F, 1e-6F);
    TEST_NEAR(pillarHeight(0.5F), 1.68F, 1e-6F);
    TEST_NEAR(pillarHeight(std::numeric_limits<float>::quiet_NaN()), 0.08F, 1e-6F);
    TEST_NEAR(pillarHeight(9), 3.28F, 1e-6F);
  }

  // The camera starts outside pillar 0, above the ring, looking at a point above its center.
  void theCameraLooksAtTheRing() {
    const PillarRing ring;
    const OrbitView view = ring.view(16.0F / 9);
    TEST_NEAR(view.eye[0], 11.0F, 1e-4F);
    TEST_NEAR(view.eye[1], 4.6F, 1e-4F);
    TEST_NEAR(view.eye[2], 0.0F, 1e-4F);
    const Clip target = project(view.viewProjection, 0, 0.9F, 0);
    TEST_NEAR(target.x, 0.0F, 1e-5F);
    TEST_NEAR(target.y, 0.0F, 1e-5F);
    // The nearest pillar stands below the middle; the ones at a quarter turn stand left and right.
    const Clip near = project(view.viewProjection, pillarX(0), 0, pillarZ(0));
    TEST_NEAR(near.x, 0.0F, 1e-5F);
    TEST_CHECK(near.y < -0.3F);
    TEST_CHECK(project(view.viewProjection, pillarX(16), 0, pillarZ(16)).x < -0.3F);
    TEST_CHECK(project(view.viewProjection, pillarX(48), 0, pillarZ(48)).x > 0.3F);
    // The far pillar is farther: it projects with a larger w.
    TEST_CHECK(project(view.viewProjection, pillarX(32), 0, pillarZ(32)).w > near.w);
    TEST_NEAR(view.right[0], 0.0F, 1e-5F);
    TEST_NEAR(view.right[2], -1.0F, 1e-5F);
    TEST_CHECK(view.up[1] > 0.9F);
  }

  // The ring's full width fits a tall box and a wide one, the tallest pillar too.
  void theRingFitsEveryBox() {
    const PillarRing ring;
    for (const float aspect : {0.75F, 1.0F, 1.8F, 2.5F}) {
      const OrbitView view = ring.view(aspect);
      for (std::size_t pillar = 0; pillar < kPillarCount; ++pillar) {
        for (const float y : {0.0F, pillarHeight(1)}) {
          const Clip clip = project(view.viewProjection, pillarX(pillar), y, pillarZ(pillar));
          TEST_CHECK(std::abs(clip.x) < 0.95F && std::abs(clip.y) < 0.95F);
        }
      }
    }
    // A box without a usable shape is treated as square.
    const OrbitView odd = ring.view(std::numeric_limits<float>::quiet_NaN());
    TEST_NEAR(project(odd.viewProjection, 0, 0.9F, 0).x, 0.0F, 1e-5F);
  }

  void pillarsDrawFromFarToNear() {
    const PillarRing ring;
    const OrbitView view = ring.view(1);
    const auto order = drawOrder(view.eye);
    std::array<bool, kPillarCount> seen{};
    float last = std::numeric_limits<float>::infinity();
    for (const std::uint8_t pillar : order) {
      TEST_CHECK(pillar < kPillarCount && !seen.at(pillar));
      seen.at(pillar) = true;
      const float distance = std::hypot(pillarX(pillar) - view.eye[0], pillarZ(pillar) - view.eye[2]);
      TEST_CHECK(distance <= last + 1e-5F);
      last = distance;
    }
    TEST_CHECK(order.front() == 32);
    TEST_CHECK(order.back() == 0);
    // Ties break by index, so one eye always gives one order.
    TEST_CHECK((drawOrder(view.eye) == order));
    // From (11, 0), pillar i lies farther than the center when cos(i / 64 turns) < 16 / 88: pillars 15 to 49.
    TEST_CHECK(pillarsBehindCore(view.eye) == 35);
    for (std::size_t slot = 0; slot < kPillarCount; ++slot) {
      const float distance = std::hypot(pillarX(order.at(slot)) - view.eye[0], pillarZ(order.at(slot)) - view.eye[2]);
      TEST_CHECK((slot < 35) == (distance > 11.0F));
    }
    // Above the center every pillar is farther; from far away, those past a quarter turn: pillars 16 to 48.
    TEST_CHECK(pillarsBehindCore({0, 4, 0}) == 64);
    TEST_CHECK(pillarsBehindCore({100, 4, 0}) == 33);
  }

  void aToneRaisesItsTwoPillars() {
    const PillarRing ring = played(AudioScene::Tone, 60);
    TEST_NEAR(ring.levels()[15], 0.9F, 1e-5F);
    TEST_NEAR(ring.levels()[48], 0.9F, 1e-5F);
    TEST_NEAR(ring.levels()[16], 0.0F, 1e-6F);
    TEST_NEAR(ring.levels()[0], 0.0F, 1e-6F);
    // An average bass swells the core from 0.7 to 1.05.
    TEST_NEAR(played(AudioScene::Tone, 240).coreRadius(), 1.05F, 1e-3F);
    TEST_NEAR(played(AudioScene::Silence, 240).coreRadius(), 0.7F, 1e-6F);
  }

  // Silence drifts once around in 32 s; kicks push the camera on and dip it toward the ring.
  void theCameraCirclesOnTheBeat() {
    const PillarRing quiet = played(AudioScene::Silence, 60);
    TEST_NEAR(eyeAngle(quiet), 2 * std::numbers::pi_v<float> / 32, 1e-4F);
    TEST_NEAR(quiet.pulse(), 0.0F, 1e-6F);
    const PillarRing kicked = played(AudioScene::Kicks, 60);
    TEST_CHECK(eyeAngle(kicked) > eyeAngle(quiet) * 1.2F);

    PillarRing ring;
    AudioFrameSource source(AudioScene::Kicks);
    ring.advance(source.next());
    TEST_NEAR(ring.pulse(), 1.0F, 1e-6F);
    const OrbitView struck = ring.view(1);
    for (int frame = 1; frame < 29; ++frame) {
      ring.advance(source.next());
    }
    const OrbitView later = ring.view(1);
    TEST_CHECK(ring.pulse() < 0.15F);
    const float struckDistance = std::hypot(struck.eye[0], struck.eye[2]);
    const float laterDistance = std::hypot(later.eye[0], later.eye[2]);
    TEST_NEAR(struckDistance, 10.4F, 1e-3F);
    TEST_CHECK(laterDistance > 10.95F);
    TEST_CHECK(struck.eye[1] < later.eye[1] - 0.2F);
  }

  void theCoreAndStarsTurn() {
    PillarRing ring = played(AudioScene::Silence, 60);
    TEST_NEAR(ring.coreTurn()[0], 0.3F, 1e-4F);
    TEST_NEAR(ring.coreTurn()[1], 0.17F, 1e-4F);
    TEST_NEAR(ring.starTurn(), -0.01F, 1e-5F);
    AudioFrameSource source(AudioScene::Silence);
    for (int frame = 0; frame < 3600; ++frame) {
      ring.advance(source.next());
    }
    // A minute later the camera still circles at its distance and between its heights.
    const OrbitView view = ring.view(1);
    TEST_NEAR(std::hypot(view.eye[0], view.eye[2]), 11.0F, 1e-3F);
    TEST_CHECK(view.eye[1] >= 3.4F - 1e-4F && view.eye[1] <= 5.8F + 1e-4F);
    TEST_CHECK(ring.coreTurn()[0] >= 0.0F && ring.coreTurn()[0] < 2 * std::numbers::pi_v<float>);
  }

  void hostileFramesStayFinite() {
    PillarRing ring;
    AudioFrameSource source(AudioScene::Hostile);
    for (int frame = 0; frame < 40; ++frame) {
      ring.advance(source.next());
      for (const float level : ring.levels()) {
        TEST_CHECK(level >= 0.0F && level <= 1.0F);
      }
      TEST_CHECK(ring.pulse() >= 0.0F && ring.pulse() <= 1.0F);
      TEST_CHECK(ring.coreRadius() >= 0.7F && ring.coreRadius() <= 1.4F);
      const OrbitView view = ring.view(1.5F);
      TEST_CHECK(std::ranges::all_of(view.viewProjection, [](float v) { return std::isfinite(v); }));
    }
  }

  void resetForgetsEverything() {
    PillarRing ring = played(AudioScene::Kicks, 45);
    ring.reset();
    TEST_NEAR(ring.pulse(), 0.0F, 1e-6F);
    TEST_NEAR(ring.levels()[0], 0.0F, 1e-6F);
    TEST_NEAR(ring.view(1).eye[0], 11.0F, 1e-4F);
  }
} // namespace

int main() {
  bandsFoldOverTheRing();
  theCameraLooksAtTheRing();
  theRingFitsEveryBox();
  pillarsDrawFromFarToNear();
  aToneRaisesItsTwoPillars();
  theCameraCirclesOnTheBeat();
  theCoreAndStarsTurn();
  hostileFramesStayFinite();
  resetForgetsEverything();
  std::println("pillar ring checks passed");
  return 0;
}
