#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/audio_history.h"
#include "workbench/stargate_motion.h"
#include "workbench/stargate_rows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <print>
#include <stdexcept>
#include <vector>

namespace {
  using luciole::workbench::AudioFrame;
  using luciole::workbench::AudioHistory;
  using luciole::workbench::HistoryChange;
  using luciole::workbench::kStargateLanes;
  using luciole::workbench::kStargateRows;
  using luciole::workbench::StargateFlight;
  using luciole::workbench::StargateRowBuilder;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  using Row = std::array<std::uint8_t, kStargateLanes * 4>;

  Row build(StargateRowBuilder& builder, const AudioFrame& frame) {
    Row row{};
    builder.build(frame, row);
    return row;
  }

  int channel(const Row& row, std::size_t lane, std::size_t index) { return row[(lane * 4) + index]; }

  // Successive windows of a steady tone start at a rising zero crossing, so its lanes stay in place from row to row.
  void toneLanesStayInPlace() {
    StargateRowBuilder builder;
    AudioFrameSource source(AudioScene::Tone);
    Row previous = build(builder, source.next());
    for (int i = 0; i < 20; ++i) {
      const Row row = build(builder, source.next());
      int largest = 0;
      for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
        largest = std::max(largest, std::abs(channel(row, lane, 0) - channel(previous, lane, 0)));
      }
      // One sample of a 50-sample period moves a full-range lane by at most an eighth of the range.
      TEST_CHECK(largest <= 34);
      previous = row;
    }
    // The first lane is the crossing itself: the middle of the range, rising.
    TEST_CHECK(std::abs(channel(previous, 0, 0) - 128) <= 24);
    TEST_CHECK(channel(previous, 1, 0) > channel(previous, 0, 0));
  }

  // The tone of amplitude 0.6 is lifted to fill the range; the streak holds each lane's size; the bands give the
  // hue and the energy of the whole row.
  void toneFillsTheRange() {
    StargateRowBuilder builder;
    AudioFrameSource source(AudioScene::Tone);
    Row row{};
    for (int i = 0; i < 10; ++i) {
      row = build(builder, source.next());
    }
    int lowest = 255;
    int highest = 0;
    int streaks = 0;
    for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
      lowest = std::min(lowest, channel(row, lane, 0));
      highest = std::max(highest, channel(row, lane, 0));
      streaks += channel(row, lane, 1);
      TEST_CHECK(channel(row, lane, 1) + 2 >= std::abs((channel(row, lane, 0) * 2) - 255));
      TEST_CHECK(channel(row, lane, 2) == 121); // 30 / 63 of 255
      TEST_CHECK(channel(row, lane, 3) == 4);   // 0.9 / 64 of 255
    }
    TEST_CHECK(lowest <= 8);
    TEST_CHECK(highest >= 247);
    // A sine's mean size is 2 / pi of its peak.
    TEST_NEAR(static_cast<double>(streaks) / kStargateLanes / 255, 2 / std::numbers::pi, 0.08);
  }

  // A very quiet signal is lifted at most twelve and a half times.
  void quietSignalStaysQuiet() {
    StargateRowBuilder builder;
    std::vector<float> wave(luciole::workbench::kAudioWaveformSamples);
    for (std::size_t i = 0; i < wave.size(); ++i) {
      wave[i] = 0.02F * static_cast<float>(std::sin(static_cast<double>(i) * 0.3));
    }
    const AudioFrame frame{.waveform = wave, .bands = {}};
    const Row row = build(builder, frame);
    int highest = 0;
    for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
      highest = std::max(highest, channel(row, lane, 0));
    }
    // 0.5 + 0.02 * 12.5 / 2 of 255.
    TEST_CHECK(highest <= 160 && highest >= 150);
  }

  // Silence writes flat rows whose streaks fade.
  void silenceFades() {
    StargateRowBuilder builder;
    AudioFrameSource tone(AudioScene::Tone);
    for (int i = 0; i < 5; ++i) {
      (void)build(builder, tone.next());
    }
    AudioFrameSource silence(AudioScene::Silence);
    Row row = build(builder, silence.next());
    for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
      TEST_CHECK(channel(row, lane, 0) == 128);
      TEST_CHECK(channel(row, lane, 2) == 128);
      TEST_CHECK(channel(row, lane, 3) == 0);
    }
    for (int i = 0; i < 80; ++i) {
      row = build(builder, silence.next());
    }
    for (std::size_t lane = 0; lane < kStargateLanes; ++lane) {
      TEST_CHECK(channel(row, lane, 1) <= 1);
    }
  }

  void hostileFramesGiveRows() {
    StargateRowBuilder builder;
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 12; ++i) {
      const Row row = build(builder, source.next());
      TEST_CHECK(std::ranges::any_of(row, [](std::uint8_t byte) { return byte != 0; }));
    }
    std::array<std::uint8_t, 12> tooShort{};
    bool refused = false;
    try {
      builder.build(source.next(), tooShort);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
  }

  int pushesOver(AudioScene scene, int frames, StargateFlight& flight, AudioHistory& history) {
    AudioFrameSource source(scene);
    int pushes = 0;
    for (int i = 0; i < frames; ++i) {
      flight.advance(source.next(), history);
      const auto update = history.takeUpdate();
      TEST_CHECK(update.change != HistoryChange::All);
      if (update.change == HistoryChange::Row) {
        TEST_CHECK(update.row == flight.head());
        ++pushes;
      }
    }
    return pushes;
  }

  // At the base speed a row takes 3.6 s from the horizon to the eye: 128 rows in 3.6 s. Heavy bass travels faster.
  void flightPushesRowsAtItsSpeed() {
    StargateFlight calm;
    AudioHistory calmHistory(kStargateLanes, kStargateRows);
    (void)calmHistory.takeUpdate();
    const int calmPushes = pushesOver(AudioScene::Silence, 360, calm, calmHistory);
    TEST_CHECK(std::abs(calmPushes - 213) <= 1); // 128 / 3.6 * 6
    StargateFlight driven;
    AudioHistory drivenHistory(kStargateLanes, kStargateRows);
    (void)drivenHistory.takeUpdate();
    const int drivenPushes = pushesOver(AudioScene::Kicks, 360, driven, drivenHistory);
    TEST_CHECK(drivenPushes > calmPushes + 20);
    TEST_CHECK(driven.roll() > 0 && calm.roll() > 0);
    TEST_CHECK(driven.fraction() >= 0 && driven.fraction() < 1);
    driven.reset();
    TEST_CHECK(driven.head() == 0 && driven.roll() == 0 && driven.pulse() == 1);
  }

  void flightRefusesAnotherHistoryShape() {
    StargateFlight flight;
    AudioHistory wrong(kStargateLanes, 64);
    AudioFrameSource source(AudioScene::Tone);
    bool refused = false;
    try {
      flight.advance(source.next(), wrong);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
  }
} // namespace

int main() {
  toneLanesStayInPlace();
  toneFillsTheRange();
  quietSignalStaysQuiet();
  silenceFades();
  hostileFramesGiveRows();
  flightPushesRowsAtItsSpeed();
  flightRefusesAnotherHistoryShape();
  std::println("stargate rows checks passed");
  return 0;
}
