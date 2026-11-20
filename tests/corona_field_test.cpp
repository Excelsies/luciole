#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/corona_field.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>

// The state behind the Eclipse visual: band levels around the ring, the bass swell and the flares of hits.
namespace {
  using luciole::workbench::AudioFrame;
  using luciole::workbench::CoronaField;
  using luciole::workbench::fitCorona;
  using luciole::workbench::kAudioBandCount;
  using luciole::workbench::kCoronaDiscRadius;
  using luciole::workbench::kCoronaRingWidth;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  void run(CoronaField& field, AudioFrameSource& source, int frames) {
    for (int i = 0; i < frames; ++i) {
      field.advance(source.next());
    }
  }

  void startsAtRest() {
    const CoronaField field;
    for (const float level : field.levels()) {
      TEST_CHECK(level == 0);
    }
    TEST_CHECK(field.swell() == 0);
    TEST_NEAR(field.discRadius(), 0.44F, 1e-6F);
    TEST_NEAR(field.outerRadius(), 0.62F, 1e-6F);
    TEST_NEAR(kCoronaDiscRadius + kCoronaRingWidth, 0.62F, 1e-6F);
    for (const auto& flare : field.flares()) {
      TEST_CHECK(flare.amplitude == 0);
    }
    TEST_CHECK(field.turn() == 0);
    TEST_CHECK(field.clock() == 0);
  }

  // Band 30 alone at 0.9: its level settles there and spreads two bands each way, a third less per band.
  void toneLightsAWedge() {
    CoronaField field;
    AudioFrameSource source(AudioScene::Tone);
    run(field, source, 120);
    const auto levels = field.levels();
    TEST_NEAR(levels[30], 0.9F, 0.002F);
    TEST_NEAR(levels[29], 0.6F, 0.002F);
    TEST_NEAR(levels[31], 0.6F, 0.002F);
    TEST_NEAR(levels[28], 0.3F, 0.002F);
    TEST_NEAR(levels[32], 0.3F, 0.002F);
    TEST_CHECK(levels[27] == 0);
    TEST_CHECK(levels[33] == 0);
    TEST_CHECK(levels[0] == 0);
    TEST_CHECK(levels[kAudioBandCount - 1] == 0);
    // Bass 1 drives the swell to a third.
    TEST_NEAR(field.swell(), 1.0F / 3, 0.002F);
  }

  // Levels rise with a 50 ms time constant and fall with a 350 ms one.
  void attackIsFasterThanRelease() {
    CoronaField field;
    std::array<float, kAudioBandCount> bands{};
    bands[5] = 1;
    AudioFrame frame{.waveform = {}, .bands = bands, .deltaSeconds = 1.0F / 60};
    field.advance(frame);
    // 1 - exp(-1/3)
    TEST_NEAR(field.levels()[5], 0.283469F, 1e-5F);
    bands[5] = 0;
    field.advance(frame);
    // 0.283469 * exp(-1/21)
    TEST_NEAR(field.levels()[5], 0.270287F, 1e-5F);
  }

  // A hit launches a flare at the ring that travels out at half a unit per second and fades in 0.45 s.
  void hitsLaunchFlares() {
    CoronaField field;
    AudioFrameSource source(AudioScene::Kicks);
    run(field, source, 1);
    TEST_NEAR(field.flares()[0].amplitude, 1.0F, 1e-6F);
    TEST_CHECK(field.flares()[0].distance == 0);
    const float swellAtHit = field.swell();
    run(field, source, 15);
    TEST_NEAR(field.flares()[0].distance, 0.125F, 1e-5F);
    // exp(-0.25 / 0.45)
    TEST_NEAR(field.flares()[0].amplitude, 0.573753F, 1e-4F);
    TEST_CHECK(field.flares()[1].amplitude == 0);
    TEST_CHECK(field.swell() > swellAtHit);
    run(field, source, 15);
    // The second hit takes a free slot while the first flare travels on.
    TEST_NEAR(field.flares()[1].amplitude, 1.0F, 1e-6F);
    TEST_NEAR(field.flares()[0].distance, 0.25F, 1e-5F);
    // A hit swells the ring outward and shrinks the disc.
    TEST_CHECK(field.discRadius() < kCoronaDiscRadius);
    TEST_CHECK(field.outerRadius() > kCoronaDiscRadius + kCoronaRingWidth);
    // Kicks fill the lowest bands: the corona grows at the bottom of the ring only.
    TEST_CHECK(field.levels()[0] > 0.3F);
    TEST_CHECK(field.levels()[20] == 0);
    // Hits keep coming: at most four flares live, each still inside the reach.
    run(field, source, 600);
    int live = 0;
    for (const auto& flare : field.flares()) {
      live += flare.amplitude > 0 ? 1 : 0;
      TEST_CHECK(flare.distance >= 0 && flare.distance <= 1);
    }
    TEST_CHECK(live == 4);
  }

  void silenceSettles() {
    CoronaField field;
    AudioFrameSource kicks(AudioScene::Kicks);
    run(field, kicks, 60);
    AudioFrameSource silence(AudioScene::Silence);
    run(field, silence, 600);
    for (const float level : field.levels()) {
      TEST_CHECK(level < 1e-6F);
    }
    TEST_CHECK(field.swell() < 1e-6F);
    TEST_NEAR(field.discRadius(), kCoronaDiscRadius, 1e-5F);
    for (const auto& flare : field.flares()) {
      TEST_CHECK(flare.amplitude == 0);
    }
  }

  void driftsSlowly() {
    CoronaField field;
    AudioFrameSource source(AudioScene::Silence);
    run(field, source, 600);
    TEST_NEAR(field.turn(), 0.3F, 1e-4F);
    TEST_NEAR(field.clock(), 10.0F, 1e-3F);
  }

  void hostileFramesStayInRange() {
    CoronaField field;
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 400; ++i) {
      field.advance(source.next());
      for (const float level : field.levels()) {
        TEST_CHECK(std::isfinite(level) && level >= 0 && level <= 1);
      }
      TEST_CHECK(std::isfinite(field.swell()) && field.swell() >= 0 && field.swell() <= 1);
      TEST_CHECK(field.discRadius() > 0 && field.outerRadius() > field.discRadius() && field.outerRadius() < 1);
      for (const auto& flare : field.flares()) {
        TEST_CHECK(std::isfinite(flare.distance) && flare.distance >= 0 && flare.distance <= 1);
        TEST_CHECK(std::isfinite(flare.amplitude) && flare.amplitude >= 0 && flare.amplitude <= 1);
      }
      TEST_CHECK(std::isfinite(field.turn()) && field.turn() >= 0 && field.turn() < 6.2832F);
      TEST_CHECK(std::isfinite(field.clock()) && field.clock() >= 0 && field.clock() < 1000);
    }
  }

  void resetForgetsEverything() {
    CoronaField field;
    AudioFrameSource source(AudioScene::Kicks);
    run(field, source, 40);
    field.reset();
    const CoronaField fresh;
    for (std::size_t i = 0; i < kAudioBandCount; ++i) {
      TEST_CHECK(field.levels()[i] == fresh.levels()[i]);
    }
    TEST_CHECK(field.swell() == 0 && field.turn() == 0 && field.clock() == 0);
    TEST_CHECK(field.flares()[0].amplitude == 0);
  }

  void fitsTheShorterSide() {
    const auto portrait = fitCorona(450, 600);
    TEST_NEAR(portrait.centerX, 225.0F, 1e-4F);
    TEST_NEAR(portrait.centerY, 300.0F, 1e-4F);
    TEST_NEAR(portrait.radius, 211.5F, 1e-3F);
    const auto wide = fitCorona(1350, 750);
    TEST_NEAR(wide.radius, 352.5F, 1e-3F);
    const auto none = fitCorona(0, 0);
    TEST_CHECK(none.radius == 1 && none.centerX == 0 && none.centerY == 0);
    const auto broken = fitCorona(std::numeric_limits<float>::quiet_NaN(), -5);
    TEST_CHECK(broken.radius == 1 && broken.centerX == 0 && broken.centerY == 0);
  }
} // namespace

int main() {
  startsAtRest();
  toneLightsAWedge();
  attackIsFasterThanRelease();
  hitsLaunchFlares();
  silenceSettles();
  driftsSlowly();
  hostileFramesStayInRange();
  resetForgetsEverything();
  fitsTheShorterSide();
  std::println("corona field checks passed");
  return 0;
}
