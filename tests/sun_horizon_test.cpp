#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/sun_horizon.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <print>

// The state behind the Dark Sun visual: the sun's swell, the inverted range made of the spectrum, and the beam a
// hit lets fall.
namespace {
  using luciole::workbench::kHorizonColumns;
  using luciole::workbench::layoutSun;
  using luciole::workbench::SunHorizon;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  void run(SunHorizon& sun, AudioFrameSource& source, int frames) {
    for (int i = 0; i < frames; ++i) {
      sun.advance(source.next());
    }
  }

  // In silence the range rests on a fixed jagged ridge: mirrored, deepest near the middle, none at its two ends.
  void silenceKeepsTheRestingRidge() {
    SunHorizon sun;
    AudioFrameSource source(AudioScene::Silence);
    run(sun, source, 120);
    const auto profile = sun.profile();
    for (std::size_t i = 0; i < kHorizonColumns; ++i) {
      TEST_CHECK(profile[i] == profile[kHorizonColumns - 1 - i]);
      TEST_CHECK(profile[i] >= 0 && profile[i] <= 0.1F);
    }
    TEST_CHECK(profile[0] == 0);
    TEST_CHECK(profile[31] > 0.03F);
    TEST_CHECK(profile[31] != profile[30]);
    TEST_CHECK(sun.swell() == 0);
    TEST_NEAR(sun.rim(), 0.5F, 1e-6F);
    TEST_NEAR(sun.reach(), 0.6F, 1e-6F);
    TEST_CHECK(sun.surge() == 0);
    TEST_NEAR(sun.glow(), 0.5F, 1e-6F);
  }

  // Band 30 at 0.9 hangs as two peaks, half a range from the middle on each side: 0.9 squared, times the shoulder of
  // that column.
  void toneHangsTwoPeaks() {
    SunHorizon sun;
    AudioFrameSource source(AudioScene::Tone);
    run(sun, source, 120);
    const auto profile = sun.profile();
    // 0.81 * (0.5 + 0.5 * 16 / 31.5)
    TEST_NEAR(profile[16], 0.610714F, 0.002F);
    TEST_NEAR(profile[47], 0.610714F, 0.002F);
    for (std::size_t i = 0; i < kHorizonColumns; ++i) {
      if (i != 16 && i != 47) {
        TEST_CHECK(profile[i] <= 0.1F);
      }
    }
    // Bass 1 gives a swell of 0.4; the steady tone lets no beam fall.
    TEST_NEAR(sun.swell(), 0.4F, 0.002F);
    TEST_NEAR(sun.rim(), 0.7F, 0.002F);
    TEST_NEAR(sun.reach(), 1.0F, 0.002F);
    TEST_CHECK(sun.surge() == 0);
  }

  // A kick fills the middle columns, which hold their peak for 0.3 s, and lets a beam fall that is mostly gone after
  // half a second.
  void kicksDropTheBeam() {
    SunHorizon sun;
    AudioFrameSource source(AudioScene::Kicks);
    run(sun, source, 1);
    TEST_NEAR(sun.surge(), 1.0F, 1e-6F);
    run(sun, source, 5);
    const float deepest = sun.profile()[31];
    TEST_CHECK(deepest > 0.5F);
    TEST_CHECK(sun.profile()[32] == deepest);
    run(sun, source, 10);
    // exp(-7 * 15 / 60)
    TEST_NEAR(sun.surge(), 0.173774F, 1e-4F);
    TEST_CHECK(sun.profile()[31] >= 0.95F * deepest);
    run(sun, source, 14);
    TEST_CHECK(sun.surge() < 0.04F);
    TEST_CHECK(sun.profile()[31] < 0.9F * deepest);
    // Frame 30 is the next kick, which swells the sun.
    const float swellBefore = sun.swell();
    run(sun, source, 1);
    TEST_NEAR(sun.surge(), 1.0F, 1e-6F);
    TEST_CHECK(sun.swell() > swellBefore + 0.02F);
    // The highest bands stay quiet: the outer columns keep the resting ridge.
    TEST_CHECK(sun.profile()[4] <= 0.1F);
  }

  // The bright point travels around the rim at 0.12 rad/s, from low on the right.
  void diamondTravels() {
    SunHorizon sun;
    TEST_NEAR(sun.diamond(), 5.43319F, 1e-4F);
    AudioFrameSource source(AudioScene::Silence);
    run(sun, source, 600);
    TEST_NEAR(sun.diamond(), 0.35F, 1e-3F);
    SunHorizon loud;
    AudioFrameSource kicks(AudioScene::Kicks);
    run(loud, kicks, 600);
    const float traveled = std::remainder(loud.diamond() - 5.43319F, 6.2831853F);
    TEST_CHECK(traveled > 1.3F && traveled < 2.4F);
  }

  void hostileFramesStayInRange() {
    SunHorizon sun;
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 400; ++i) {
      sun.advance(source.next());
      for (const float depth : sun.profile()) {
        TEST_CHECK(std::isfinite(depth) && depth >= 0 && depth <= 1);
      }
      TEST_CHECK(std::isfinite(sun.swell()) && sun.swell() >= 0 && sun.swell() <= 1);
      TEST_CHECK(sun.rim() >= 0.5F && sun.rim() <= 1);
      TEST_CHECK(sun.reach() >= 0.6F && sun.reach() <= 1.6F);
      TEST_CHECK(sun.surge() >= 0 && sun.surge() <= 1);
      TEST_CHECK(sun.glow() >= 0.5F && sun.glow() <= 1.5F);
      TEST_CHECK(std::isfinite(sun.diamond()) && sun.diamond() >= 0 && sun.diamond() < 6.2832F);
      TEST_CHECK(std::isfinite(sun.coronaTurn()) && sun.coronaTurn() >= 0 && sun.coronaTurn() < 6.2832F);
    }
  }

  void resetForgetsEverything() {
    SunHorizon sun;
    AudioFrameSource source(AudioScene::Kicks);
    run(sun, source, 40);
    sun.reset();
    const SunHorizon fresh;
    for (std::size_t i = 0; i < kHorizonColumns; ++i) {
      TEST_CHECK(sun.profile()[i] == fresh.profile()[i]);
    }
    TEST_CHECK(sun.swell() == 0 && sun.surge() == 0 && sun.diamond() == fresh.diamond());
  }

  // The composition of a 450 by 600 rectangle: horizon at half height, a small sun high in the sky, the range half
  // the width across.
  void laysOutTheScene() {
    const auto rest = layoutSun(450, 600, 0);
    TEST_NEAR(rest.horizon, 300.0F, 1e-4F);
    TEST_NEAR(rest.sunX, 225.0F, 1e-4F);
    TEST_NEAR(rest.sunY, 132.0F, 1e-4F);
    TEST_NEAR(rest.sunRadius, 38.25F, 1e-4F);
    TEST_NEAR(rest.rangeHalf, 112.5F, 1e-4F);
    TEST_NEAR(rest.rangeDepth, 126.0F, 1e-4F);
    TEST_NEAR(rest.beamWidth, 1.8F, 1e-4F);
    // A full swell grows the sun by a sixth.
    TEST_NEAR(layoutSun(450, 600, 1).sunRadius, 44.625F, 1e-4F);
    // A flat rectangle keeps the sun inside its top edge and the range above its bottom edge.
    const auto flat = layoutSun(900, 100, 1);
    TEST_CHECK(flat.sunY >= flat.sunRadius);
    TEST_CHECK(flat.rangeDepth <= 0.8F * 50);
    const auto none = layoutSun(std::numeric_limits<float>::quiet_NaN(), -1, 0.5F);
    TEST_CHECK(none.horizon == 0 && none.sunRadius == 0 && none.rangeHalf == 0 && none.rangeDepth == 0);
  }
} // namespace

int main() {
  silenceKeepsTheRestingRidge();
  toneHangsTwoPeaks();
  kicksDropTheBeam();
  diamondTravels();
  hostileFramesStayInRange();
  resetForgetsEverything();
  laysOutTheScene();
  std::println("sun horizon checks passed");
  return 0;
}
