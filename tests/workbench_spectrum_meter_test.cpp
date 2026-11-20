#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/spectrum_meter.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr float kStep = 1.0F / 60;

  void easeMovesByTheKeptShareEachSixtiethOfASecond() {
    TEST_NEAR(easeToward(1.0F, 0.0F, 0.5F, kStep), 0.5F, 1e-6F);
    TEST_NEAR(easeToward(0.0F, 1.0F, 0.5F, kStep), 0.5F, 1e-6F);
    // Two short steps land where one step of their length does.
    TEST_NEAR(easeToward(easeToward(1.0F, 0.0F, 0.5F, kStep), 0.0F, 0.5F, kStep), 0.25F, 1e-6F);
    TEST_NEAR(easeToward(1.0F, 0.0F, 0.5F, 2 * kStep), 0.25F, 1e-6F);
    TEST_NEAR(easeToward(0.3F, 0.9F, 0.0F, kStep), 0.9F, 1e-6F);
    TEST_NEAR(easeToward(0.3F, 0.9F, 0.7F, 0.0F), 0.3F, 1e-6F);
  }

  void easeRejectsWhatIsNotANumber() {
    constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    TEST_NEAR(easeToward(kNan, 0.5F, 0.5F, kStep), 0.5F, 1e-6F);
    TEST_NEAR(easeToward(0.5F, kNan, 0.5F, kStep), 0.25F, 1e-6F);
    TEST_NEAR(easeToward(1.0F, 0.0F, kNan, kStep), 0.0F, 1e-6F);
    TEST_NEAR(easeToward(1.0F, 0.0F, 0.5F, kNan), 1.0F, 1e-6F);
    // A keep of 1 or more would never move: it is held just below 1.
    TEST_CHECK(easeToward(1.0F, 0.0F, 2.0F, kStep) < 1.0F);
    TEST_CHECK(std::isfinite(easeToward(1.0F, 0.0F, 0.5F, kInfinity)));
  }

  void levelsRiseAtOnceAndFallByTheKeptShare() {
    std::array<float, 2> levels{1.0F, 0.0F};
    const std::array<float, 2> targets{0.0F, 0.8F};
    smoothLevels(levels, targets, 0.5F, kStep);
    TEST_NEAR(levels[0], 0.5F, 1e-6F);
    TEST_NEAR(levels[1], 0.8F, 1e-6F);
    // Only the shorter span is touched.
    std::array<float, 3> longer{1.0F, 1.0F, 1.0F};
    smoothLevels(longer, std::array<float, 1>{0.0F}, 0.0F, kStep);
    TEST_NEAR(longer[0], 0.0F, 1e-6F);
    TEST_NEAR(longer[2], 1.0F, 1e-6F);
  }

  void groupsTakeTheLoudestBand() {
    std::array<float, 64> ramp{};
    for (std::size_t i = 0; i < ramp.size(); ++i) {
      ramp[i] = static_cast<float>(i) / 63;
    }
    std::array<float, 16> sixteen{};
    groupLoudest(ramp, sixteen);
    TEST_NEAR(sixteen[0], 3.0F / 63, 1e-6F);
    TEST_NEAR(sixteen[15], 1.0F, 1e-6F);
    std::array<float, 64> same{};
    groupLoudest(ramp, same);
    TEST_NEAR(same[17], 17.0F / 63, 1e-6F);
    // Groups that do not divide the input still cover all of it: {0.1, 0.9}, {0.2, 0.3}, {0.8, 0.4, 0.7}.
    std::array<float, 3> three{};
    groupLoudest(std::array<float, 7>{0.1F, 0.9F, 0.2F, 0.3F, 0.8F, 0.4F, 0.7F}, three);
    TEST_NEAR(three[0], 0.9F, 1e-6F);
    TEST_NEAR(three[1], 0.3F, 1e-6F);
    TEST_NEAR(three[2], 0.8F, 1e-6F);
    std::array<float, 4> fromNothing{1, 1, 1, 1};
    groupLoudest(std::span<const float>{}, fromNothing);
    TEST_NEAR(fromNothing[3], 0.0F, 1e-6F);
  }

  void capsJumpHoldThenFallToTheLevel() {
    PeakCaps caps;
    caps.advance(std::array<float, 1>{0.9F}, kStep);
    TEST_NEAR(caps.peaks()[0], 0.9F, 1e-6F);
    const std::array<float, 1> low{0.2F};
    // Held for 0.45 s: 26 frames of a 60th of a second.
    for (int frame = 0; frame < 26; ++frame) {
      caps.advance(low, kStep);
    }
    TEST_NEAR(caps.peaks()[0], 0.9F, 1e-6F);
    for (int frame = 0; frame < 3; ++frame) {
      caps.advance(low, kStep);
    }
    const float falling = caps.peaks()[0];
    TEST_CHECK(falling < 0.9F && falling > 0.85F);
    // Falling with gravity: each step drops more than the one before.
    caps.advance(low, kStep);
    const float next = caps.peaks()[0];
    caps.advance(low, kStep);
    TEST_CHECK(falling - next < next - caps.peaks()[0]);
    for (int frame = 0; frame < 60; ++frame) {
      caps.advance(low, kStep);
    }
    TEST_NEAR(caps.peaks()[0], 0.2F, 1e-6F);
    // A new high takes the cap back up at once.
    caps.advance(std::array<float, 1>{0.6F}, kStep);
    TEST_NEAR(caps.peaks()[0], 0.6F, 1e-6F);
    caps.reset();
    TEST_NEAR(caps.peaks()[0], 0.0F, 1e-6F);
    TEST_CHECK(caps.peaks().size() == kAudioBandCount);
  }

  void bandsAreReadInRange() {
    test::audio::AudioFrameSource hostile(test::audio::AudioScene::Hostile);
    for (int frame = 0; frame < 8; ++frame) {
      std::array<float, kAudioBandCount> bands{};
      readBands(hostile.next(), bands);
      for (const float band : bands) {
        TEST_CHECK(band >= 0.0F && band <= 1.0F);
      }
    }
    test::audio::AudioFrameSource tone(test::audio::AudioScene::Tone);
    std::array<float, kAudioBandCount> bands{};
    readBands(tone.next(), bands);
    TEST_NEAR(bands[test::audio::kToneBand], 0.9F, 1e-6F);
    TEST_NEAR(bands[0], 0.0F, 1e-6F);
  }
} // namespace

int main() {
  easeMovesByTheKeptShareEachSixtiethOfASecond();
  easeRejectsWhatIsNotANumber();
  levelsRiseAtOnceAndFallByTheKeptShare();
  groupsTakeTheLoudestBand();
  capsJumpHoldThenFallToTheLevel();
  bandsAreReadInRange();
  std::println("spectrum meter checks passed");
  return 0;
}
