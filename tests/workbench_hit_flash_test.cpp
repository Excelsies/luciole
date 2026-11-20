#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/hit_flash.h"

#include <cmath>
#include <limits>
#include <print>

namespace {
  using namespace luciole::workbench;

  constexpr float kStep = 1.0F / 60;

  void anIsolatedFlashPassesAtOnce() {
    FlashLimiter limiter;
    TEST_NEAR(limiter.step(0.2F, kStep), 0.2F, 1e-6F);
    TEST_NEAR(limiter.step(1.0F, kStep), 1.0F, 1e-6F);
    // A fall passes at once too.
    TEST_NEAR(limiter.step(0.3F, kStep), 0.3F, 1e-6F);
    TEST_NEAR(limiter.value(), 0.3F, 1e-6F);
  }

  void aRiseUnderWayKeepsRising() {
    FlashLimiter limiter;
    for (const float target : {0.2F, 0.5F, 0.8F, 1.0F}) {
      TEST_NEAR(limiter.step(target, kStep), target, 1e-6F);
    }
  }

  // A 10 Hz strobe, bright for three frames and dark for three, shows at most three flashes a second.
  void aStrobeIsHeldToThreeFlashesASecond() {
    FlashLimiter limiter;
    int flashes = 0;
    float shown = 0;
    for (int frame = 0; frame < 60; ++frame) {
      const float target = (frame / 3) % 2 == 0 ? 1.0F : 0.0F;
      const float next = limiter.step(target, kStep);
      flashes += next > shown ? 1 : 0;
      shown = next;
    }
    TEST_CHECK(flashes >= 2 && flashes <= 3);
    // Two hits half a second apart both flash.
    FlashLimiter kicks;
    int kicked = 0;
    shown = 0;
    for (int frame = 0; frame < 60; ++frame) {
      const float next = kicks.step(frame % 30 == 0 ? 1.0F : 0.0F, kStep);
      kicked += next > shown ? 1 : 0;
      shown = next;
    }
    TEST_CHECK(kicked == 2);
  }

  void limiterRejectsWhatIsNotANumber() {
    FlashLimiter limiter;
    TEST_NEAR(limiter.step(std::numeric_limits<float>::quiet_NaN(), kStep), 0.0F, 1e-6F);
    TEST_NEAR(limiter.step(7.0F, std::numeric_limits<float>::quiet_NaN()), 1.0F, 1e-6F);
    TEST_NEAR(limiter.step(-3.0F, 1e9F), 0.0F, 1e-6F);
    limiter.step(1.0F, kStep);
    limiter.reset();
    TEST_NEAR(limiter.value(), 0.0F, 1e-6F);
    TEST_NEAR(limiter.step(0.7F, kStep), 0.7F, 1e-6F);
  }

  void envelopeDecaysUnlessKicked() {
    TEST_NEAR(kickEnvelope(1.0F, 0.0F, 10.0F, 0.1F), 0.36787944F, 1e-6F);
    // A step is never longer than a frame can be.
    TEST_NEAR(kickEnvelope(1.0F, 0.0F, 10.0F, 5.0F), 0.36787944F, 1e-6F);
    TEST_NEAR(kickEnvelope(0.1F, 0.6F, 5.0F, 0.1F), 0.6F, 1e-6F);
    TEST_NEAR(kickEnvelope(0.9F, 0.6F, 5.0F, 0.0F), 0.9F, 1e-6F);
    TEST_NEAR(kickEnvelope(std::numeric_limits<float>::quiet_NaN(), 0.0F, 5.0F, kStep), 0.0F, 1e-6F);
    TEST_NEAR(kickEnvelope(0.5F, std::numeric_limits<float>::infinity(), 5.0F, kStep), 1.0F, 1e-6F);
    TEST_NEAR(kickEnvelope(0.5F, 0.0F, 5.0F, std::numeric_limits<float>::quiet_NaN()), 0.5F, 1e-6F);
  }

  void strengthFollowsTheBassOfAHit() {
    AudioFrame frame;
    TEST_NEAR(hitStrength(frame), 0.0F, 1e-6F);
    frame.hit = true;
    frame.bass = 1;
    TEST_NEAR(hitStrength(frame), 0.4F, 1e-6F);
    frame.bass = 2;
    TEST_NEAR(hitStrength(frame), 0.7F, 1e-6F);
    frame.bass = 4;
    TEST_NEAR(hitStrength(frame), 1.0F, 1e-6F);
    frame.bass = std::numeric_limits<float>::quiet_NaN();
    TEST_NEAR(hitStrength(frame), 0.4F, 1e-6F);
    test::audio::AudioFrameSource kicks(test::audio::AudioScene::Kicks);
    TEST_NEAR(hitStrength(kicks.next()), 1.0F, 1e-6F);
    TEST_NEAR(hitStrength(kicks.next()), 0.0F, 1e-6F);
  }
} // namespace

int main() {
  anIsolatedFlashPassesAtOnce();
  aRiseUnderWayKeepsRising();
  aStrobeIsHeldToThreeFlashesASecond();
  limiterRejectsWhatIsNotANumber();
  envelopeDecaysUnlessKicked();
  strengthFollowsTheBassOfAHit();
  std::println("hit flash checks passed");
  return 0;
}
