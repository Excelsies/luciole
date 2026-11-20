#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/stargate_motion.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <print>

namespace {
  using luciole::workbench::StargateGlow;
  using luciole::workbench::StargatePulse;
  using luciole::workbench::StargateRoll;
  using luciole::workbench::StargateSchedule;
  using luciole::workbench::StargateSurge;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  constexpr float kStep = 1.0F / 60;
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInfinity = std::numeric_limits<float>::infinity();

  // At average bass the roll turns a quarter radian a second.
  void rollKeepsItsRateAtAverageBass() {
    StargateRoll roll;
    for (int i = 0; i < 120; ++i) {
      roll.step(kStep, 1);
    }
    TEST_NEAR(roll.angle(), 0.5F, 1e-4F);
  }

  // Heavy bass speeds the roll up to 1.35 times, silence slows it to 0.65 times, and neither jerks it.
  void rollBreathesWithTheBass() {
    StargateRoll heavy;
    StargateRoll quiet;
    for (int i = 0; i < 600; ++i) {
      heavy.step(kStep, 4);
      quiet.step(kStep, 0);
    }
    const float heavyBefore = heavy.angle();
    const float quietBefore = quiet.angle();
    heavy.step(kStep, 4);
    quiet.step(kStep, 0);
    TEST_NEAR((heavy.angle() - heavyBefore) / kStep, 0.25F * 1.35F, 2e-3F);
    TEST_NEAR((quiet.angle() - quietBefore) / kStep, 0.25F * 0.65F, 2e-3F);
    // The first step after a change of bass keeps nearly the old rate.
    StargateRoll roll;
    roll.step(kStep, 1);
    const float before = roll.angle();
    roll.step(kStep, 4);
    TEST_NEAR((roll.angle() - before) / kStep, 0.25F, 0.01F);
  }

  void rollIgnoresBrokenInput() {
    StargateRoll roll;
    roll.step(kNan, 1);
    roll.step(-1, 1);
    roll.step(kStep, kNan);
    roll.step(1e6F, kInfinity);
    TEST_CHECK(std::isfinite(roll.angle()));
    // A long step counts as kMaxAudioFrameSeconds.
    TEST_CHECK(std::abs(roll.angle()) < 0.1F);
  }

  void surgeFollowsHeavyBass() {
    StargateSurge surge;
    TEST_CHECK(surge.step(kStep, 1) == 1);
    TEST_CHECK(surge.step(kStep, 0.5F) == 1);
    float peak = 1;
    for (int i = 0; i < 60; ++i) {
      peak = surge.step(kStep, 3);
    }
    TEST_NEAR(peak, 1.8F, 0.01F);
    TEST_CHECK(peak <= 1.8F);
    // It eases back slowly: 0.8 * exp(-0.1 / 0.7) above 1 after a tenth of a second.
    float later = peak;
    for (int i = 0; i < 6; ++i) {
      later = surge.step(kStep, 0);
    }
    TEST_NEAR(later, 1.0F + (0.8F * std::exp(-0.1F / 0.7F)), 0.01F);
    TEST_CHECK(surge.step(kStep, kNan) <= later);
    surge.reset();
    TEST_CHECK(surge.value() == 1);
  }

  void pulseFlaresOnAHit() {
    StargatePulse pulse;
    TEST_CHECK(pulse.step(kStep, false, 1) == 1);
    TEST_NEAR(pulse.step(kStep, true, 2), 1.35F, 1e-5F);
    // A hit on quiet bass flares half as much.
    StargatePulse soft;
    TEST_NEAR(soft.step(kStep, true, 0), 1.175F, 1e-5F);
    for (int i = 0; i < 30; ++i) {
      (void)pulse.step(kStep, false, 1);
    }
    TEST_NEAR(pulse.value(), 1.0F + (0.35F * std::exp(-3.0F)), 1e-3F);
  }

  // Thirty rows a second at sixty steps a second push every other step; the fraction glides between pushes.
  void schedulePushesAtItsRate() {
    StargateSchedule schedule;
    int pushes = 0;
    for (int i = 0; i < 60; ++i) {
      if (schedule.step(kStep, 30)) {
        ++pushes;
      }
      TEST_CHECK(schedule.fraction() >= 0 && schedule.fraction() < 1);
    }
    TEST_CHECK(pushes == 30);
    TEST_CHECK(schedule.head() == 30);
    TEST_CHECK(!schedule.step(0, 30));
    TEST_CHECK(!schedule.step(kNan, 30));
    TEST_CHECK(!schedule.step(kStep, kNan));
    TEST_CHECK(!schedule.step(kStep, -5));
  }

  // However many rows fall due, one step pushes one, and the ring wraps.
  void schedulePushesOneRowAtMost() {
    StargateSchedule schedule;
    for (std::size_t i = 0; i < luciole::workbench::kStargateRows + 5; ++i) {
      TEST_CHECK(schedule.step(1e6F, 1e6F));
      TEST_CHECK(schedule.fraction() < 1);
    }
    TEST_CHECK(schedule.head() == 5);
    schedule.reset();
    TEST_CHECK(schedule.head() == 0 && schedule.fraction() == 0);
  }

  // A steady tone in band 30 of 63 colors the horizon at 30 / 63 of the ramp; its level is the band mean.
  void glowFollowsTheSpectrum() {
    StargateGlow glow;
    TEST_NEAR(glow.hue(), 0.5F, 1e-6F);
    AudioFrameSource tone(AudioScene::Tone);
    for (int i = 0; i < 300; ++i) {
      glow.step(kStep, tone.next());
    }
    TEST_NEAR(glow.hue(), 30.0F / 63, 1e-3F);
    TEST_NEAR(glow.energy(), 0.9F / 64, 1e-4F);
    TEST_NEAR(glow.drift(), 5 * 0.012F, 1e-4F);
    // Silence keeps the hue and lets the level fall.
    AudioFrameSource silence(AudioScene::Silence);
    for (int i = 0; i < 300; ++i) {
      glow.step(kStep, silence.next());
    }
    TEST_NEAR(glow.hue(), 30.0F / 63, 1e-3F);
    TEST_CHECK(glow.energy() < 1e-4F);
    AudioFrameSource hostile(AudioScene::Hostile);
    for (int i = 0; i < 40; ++i) {
      glow.step(kStep, hostile.next());
      TEST_CHECK(glow.hue() >= 0 && glow.hue() <= 1);
      TEST_CHECK(glow.energy() >= 0 && glow.energy() <= 1);
    }
  }
} // namespace

int main() {
  rollKeepsItsRateAtAverageBass();
  rollBreathesWithTheBass();
  rollIgnoresBrokenInput();
  surgeFollowsHeavyBass();
  pulseFlaresOnAHit();
  schedulePushesAtItsRate();
  schedulePushesOneRowAtMost();
  glowFollowsTheSpectrum();
  std::println("stargate motion checks passed");
  return 0;
}
