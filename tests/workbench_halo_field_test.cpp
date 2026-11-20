#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/halo_field.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <print>

namespace {
  using namespace luciole::workbench;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  std::array<std::uint8_t, 4 * kHaloTexels> rowOf(const HaloField& halo) {
    std::array<std::uint8_t, 4 * kHaloTexels> row{};
    halo.writeRow(row);
    return row;
  }

  HaloField played(AudioScene scene, int frames) {
    HaloField halo;
    AudioFrameSource source(scene);
    for (int frame = 0; frame < frames; ++frame) {
      halo.advance(source.next());
    }
    return halo;
  }

  void silenceKeepsTheRestingRing() {
    const HaloField halo = played(AudioScene::Silence, 120);
    const HaloRing ring = halo.ring();
    TEST_NEAR(ring.radius, 0.2F, 1e-5F);
    TEST_NEAR(ring.reach, 0.12F, 1e-6F);
    TEST_NEAR(ring.wobble, 0.0F, 1e-6F);
    TEST_NEAR(ring.flash, 0.0F, 1e-6F);
    std::array<std::uint8_t, 4 * kHaloTexels> row{};
    row.fill(7);
    halo.writeRow(row);
    for (std::size_t texel = 0; texel < kHaloTexels; ++texel) {
      TEST_CHECK(row.at(4 * texel) == 0 && row.at((4 * texel) + 3) == 255);
    }
  }

  void aToneSwellsItsBandAndTheBass() {
    const HaloField halo = played(AudioScene::Tone, 120);
    const HaloRing ring = halo.ring();
    // An average bass widens the ring by 0.04; the tone's level ripples it.
    TEST_NEAR(ring.radius, 0.24F, 1e-4F);
    TEST_NEAR(ring.wobble, 0.006F * 0.8485F, 1e-4F);
    const auto row = rowOf(halo);
    // Softened with its neighbors: 0.9 / 2 at its own texel, 0.9 / 4 beside it.
    TEST_CHECK(row.at(4 * test::audio::kToneBand) == 115);
    TEST_CHECK(row.at(4 * (test::audio::kToneBand + 1)) == 57);
    TEST_CHECK(row.at(4 * (test::audio::kToneBand - 1)) == 57);
    TEST_CHECK(row.at(4 * (test::audio::kToneBand + 2)) == 0);
    // A shorter row takes the first texels only.
    std::array<std::uint8_t, 6> partial{};
    halo.writeRow(partial);
    TEST_CHECK(partial[3] == 255 && partial[4] == 0);
  }

  void theRippleTurnsWithTime() {
    HaloField halo;
    AudioFrameSource source(AudioScene::Tone);
    halo.advance(source.next());
    TEST_NEAR(halo.ring().phase, 1.5F / 60, 1e-5F);
    halo.advance(source.next());
    TEST_NEAR(halo.ring().phase, 3.0F / 60, 1e-5F);
    for (int frame = 0; frame < 3000; ++frame) {
      halo.advance(source.next());
      TEST_CHECK(halo.ring().phase >= 0.0F && halo.ring().phase < 2 * std::numbers::pi_v<float>);
    }
  }

  void hitsFlashThenFade() {
    HaloField halo;
    AudioFrameSource source(AudioScene::Kicks);
    halo.advance(source.next());
    TEST_NEAR(halo.ring().flash, 1.0F, 1e-6F);
    const float struck = halo.ring().radius;
    for (int frame = 1; frame < 21; ++frame) {
      halo.advance(source.next());
    }
    // Twenty frames of decay at 6 per second.
    TEST_NEAR(halo.ring().flash, 0.13533528F, 1e-4F);
    TEST_CHECK(rowOf(halo)[0] < 26);
    for (int frame = 21; frame < 31; ++frame) {
      halo.advance(source.next());
    }
    TEST_NEAR(halo.ring().flash, 1.0F, 1e-6F);
    // Band 0 at 1 beside band 1 at 0.875, softened.
    TEST_CHECK(rowOf(halo)[0] == 247);
    TEST_CHECK(halo.ring().radius > struck);
  }

  void hostileFramesStayInRange() {
    HaloField halo;
    AudioFrameSource source(AudioScene::Hostile);
    for (int frame = 0; frame < 40; ++frame) {
      halo.advance(source.next());
      const HaloRing ring = halo.ring();
      TEST_CHECK(ring.radius >= 0.2F && ring.radius <= 0.3F);
      TEST_CHECK(ring.wobble >= 0.0F && ring.wobble <= 0.012F);
      TEST_CHECK(ring.flash >= 0.0F && ring.flash <= 1.0F);
      TEST_CHECK(std::isfinite(ring.phase));
      const auto row = rowOf(halo);
      for (std::size_t texel = 0; texel < kHaloTexels; ++texel) {
        TEST_CHECK(row.at((4 * texel) + 1) == 0 && row.at((4 * texel) + 3) == 255);
      }
    }
    halo.writeRow({});
  }

  void resetForgetsEverything() {
    HaloField halo = played(AudioScene::Kicks, 40);
    halo.reset();
    const HaloRing ring = halo.ring();
    TEST_NEAR(ring.radius, 0.2F, 1e-6F);
    TEST_NEAR(ring.phase, 0.0F, 1e-6F);
    TEST_NEAR(ring.flash, 0.0F, 1e-6F);
    TEST_CHECK(rowOf(halo)[0] == 0);
  }
} // namespace

int main() {
  silenceKeepsTheRestingRing();
  aToneSwellsItsBandAndTheBass();
  theRippleTurnsWithTime();
  hitsFlashThenFade();
  hostileFramesStayInRange();
  resetForgetsEverything();
  std::println("halo field checks passed");
  return 0;
}
