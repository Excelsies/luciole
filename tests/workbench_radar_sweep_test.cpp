#include "audio_frame_sources.h"
#include "test_check.h"
#include "workbench/audio_frame.h"
#include "workbench/audio_history.h"
#include "workbench/radar_sweep.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <print>
#include <stdexcept>
#include <vector>

namespace {
  using luciole::workbench::AudioFrame;
  using luciole::workbench::AudioHistory;
  using luciole::workbench::HistoryChange;
  using luciole::workbench::kRadarBearings;
  using luciole::workbench::kRadarRadii;
  using luciole::workbench::RadarSweep;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;

  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

  AudioHistory radarHistory() { return {kRadarRadii, kRadarBearings}; }

  // The red byte of radius texel `texel` in bearing row `row`.
  int brightnessAt(const AudioHistory& history, std::size_t row, std::size_t texel) {
    return history.texels()[(((row * kRadarRadii) + texel) * 4)];
  }
  int lapAt(const AudioHistory& history, std::size_t row) { return history.texels()[(row * kRadarRadii * 4) + 1]; }
  int markAt(const AudioHistory& history, std::size_t row) { return history.texels()[(row * kRadarRadii * 4) + 3]; }
  int steppedAt(const AudioHistory& history, std::size_t row) { return history.texels()[(row * kRadarRadii * 4) + 2]; }

  void scopeFitsTheShorterSide() {
    const auto tall = luciole::workbench::fitRadarScope(450, 600);
    TEST_NEAR(tall.centerX, 225.0F, 1e-4F);
    TEST_NEAR(tall.centerY, 300.0F, 1e-4F);
    TEST_NEAR(tall.radius, 198.0F, 1e-3F); // 225 less 12 % margin
    const auto wide = luciole::workbench::fitRadarScope(1350, 750);
    TEST_NEAR(wide.radius, 330.0F, 1e-3F);
    TEST_CHECK(luciole::workbench::fitRadarScope(0, 400).radius == 0);
    TEST_CHECK(luciole::workbench::fitRadarScope(-5, 400).radius == 0);
    TEST_CHECK(luciole::workbench::fitRadarScope(kNan, 400).radius == 0);
    TEST_CHECK(luciole::workbench::fitRadarScope(std::numeric_limits<float>::infinity(), 400).radius == 0);
  }

  // Band 30 of 63 sits at radius 0.06 + 30 / 63 * 0.89 = 0.4838 of the scope, inside texel 61 of 128.
  void echoPutsLowNotesInside() {
    AudioFrameSource source(AudioScene::Tone);
    const AudioFrame frame = source.next();
    TEST_NEAR(luciole::workbench::radarEcho(frame, 61), 0.9F, 1e-5F);
    TEST_CHECK(luciole::workbench::radarEcho(frame, 62) > 0.8F);
    TEST_CHECK(luciole::workbench::radarEcho(frame, 60) < 0.5F);
    TEST_CHECK(luciole::workbench::radarEcho(frame, 50) == 0);
    TEST_CHECK(luciole::workbench::radarEcho(frame, 75) == 0);
    // Inside the inner radius and past the outer one nothing shows, whatever the bands.
    std::vector<float> full(luciole::workbench::kAudioBandCount, 1.0F);
    const AudioFrame loud{.waveform = {}, .bands = full};
    TEST_CHECK(luciole::workbench::radarEcho(loud, 6) == 0);   // radius 0.051
    TEST_CHECK(luciole::workbench::radarEcho(loud, 8) == 1);   // radius 0.066
    TEST_CHECK(luciole::workbench::radarEcho(loud, 122) == 0); // radius 0.955
    TEST_CHECK(luciole::workbench::radarEcho(loud, kRadarRadii) == 0);
  }

  void echoOfBrokenBandsIsClamped() {
    std::vector<float> bands(luciole::workbench::kAudioBandCount, kNan);
    bands[0] = 7;
    const AudioFrame frame{.waveform = {}, .bands = bands};
    // Texel 7 starts at the inner radius, where band 0 is.
    TEST_CHECK(luciole::workbench::radarEcho(frame, 7) == 1);
    TEST_CHECK(luciole::workbench::radarEcho(frame, 60) == 0);
    std::vector<float> few(3, 1.0F);
    const AudioFrame shortFrame{.waveform = {}, .bands = few};
    TEST_CHECK(luciole::workbench::radarEcho(shortFrame, 9) == 1);
    TEST_CHECK(luciole::workbench::radarEcho(shortFrame, 40) == 0);
  }

  // A quarter turn a second: after one second at 60 frames the arm points east, in row 48 of 192.
  void armTurnsAQuarterASecond() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Tone);
    for (int i = 0; i < 60; ++i) {
      sweep.advance(source.next(), history);
    }
    TEST_NEAR(sweep.turns(), 0.25, 1e-6);
    TEST_NEAR(sweep.bearing(), 0.25F, 1e-6F);
    TEST_CHECK(sweep.armRow() == 48);
    TEST_CHECK(sweep.lap() == 0);
    for (int i = 0; i < 180; ++i) {
      sweep.advance(source.next(), history);
    }
    TEST_NEAR(sweep.turns(), 1.0, 1e-6);
    TEST_CHECK(sweep.lap() == 1);
    TEST_CHECK(sweep.armRow() == 0);
  }

  // A steady tone writes the same arc into every row the arm crosses, stamped with the lap.
  void toneWritesAnArc() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Tone);
    (void)history.takeUpdate();
    for (int i = 0; i < 60; ++i) {
      sweep.advance(source.next(), history);
      const auto update = history.takeUpdate();
      TEST_CHECK(update.change == HistoryChange::Row);
      TEST_CHECK(update.row == sweep.armRow());
    }
    for (std::size_t row = 0; row < 48; ++row) {
      TEST_CHECK(std::abs(brightnessAt(history, row, 61) - 230) <= 1); // 0.9 of 255
      TEST_CHECK(brightnessAt(history, row, 30) == 0);
      TEST_CHECK(lapAt(history, row) == 0);
      TEST_CHECK(markAt(history, row) == 255);
      TEST_CHECK(steppedAt(history, row) == 0); // less than a row a frame steps over none
    }
    // Ahead of the arm nothing was written yet.
    TEST_CHECK(brightnessAt(history, 100, 61) == 0);
    TEST_CHECK(markAt(history, 100) == 0);
    for (int i = 0; i < 240; ++i) {
      sweep.advance(source.next(), history);
    }
    TEST_CHECK(lapAt(history, 20) == 1);
    TEST_CHECK(lapAt(history, 100) == 0);
  }

  // A hit lights the whole radius under the arm, and the light falls off within a few frames.
  void hitWritesASpoke() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Kicks);
    sweep.advance(source.next(), history);
    TEST_NEAR(sweep.kick(), 1.0F, 1e-6F);
    const std::size_t hitRow = sweep.armRow();
    TEST_CHECK(brightnessAt(history, hitRow, 100) > 120);
    TEST_CHECK(brightnessAt(history, hitRow, 120) > 120);
    TEST_CHECK(brightnessAt(history, hitRow, 4) == 0);
    for (int i = 0; i < 8; ++i) {
      sweep.advance(source.next(), history);
    }
    TEST_CHECK(sweep.kick() < 0.1F);
    TEST_CHECK(brightnessAt(history, sweep.armRow(), 100) < 20);
    // The kick's low bands still sit near the center.
    TEST_CHECK(brightnessAt(history, sweep.armRow(), 9) > 20);
  }

  void silenceWritesDarkRows() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Silence);
    for (int i = 0; i < 120; ++i) {
      sweep.advance(source.next(), history);
    }
    TEST_NEAR(sweep.turns(), 0.5, 1e-6);
    for (std::size_t row = 0; row < kRadarBearings; ++row) {
      for (std::size_t texel = 0; texel < kRadarRadii; ++texel) {
        TEST_CHECK(brightnessAt(history, row, texel) == 0);
      }
    }
  }

  // The longest frame moves the arm 4.8 rows, so the next row it writes lies at most kRadarArmReach rows on.
  void longestFrameStaysWithinReach() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Tone);
    std::size_t widest = 0;
    for (int i = 0; i < 200; ++i) {
      AudioFrame frame = source.next();
      frame.deltaSeconds = 1.0F;
      const std::size_t from = sweep.armRow();
      sweep.advance(frame, history);
      const std::size_t moved = (sweep.armRow() + kRadarBearings - from) % kRadarBearings;
      widest = std::max(widest, moved);
      // The row records the rows the frame stepped over, the first frame's start row among them.
      const std::size_t stepped = i == 0 ? moved : moved - 1;
      TEST_CHECK(steppedAt(history, sweep.armRow()) == static_cast<int>(stepped));
    }
    TEST_CHECK(widest == luciole::workbench::kRadarArmReach);
  }

  // A frame that leaves the arm on its row writes it again and keeps what the frame that reached it stepped over.
  void standingStillKeepsWhatWasSteppedOver() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Tone);
    AudioFrame frame = source.next();
    frame.deltaSeconds = 1.0F / 60;
    sweep.advance(frame, history); // turn 1 / 240, still row 0
    TEST_CHECK(sweep.armRow() == 0 && steppedAt(history, 0) == 0);
    frame.deltaSeconds = 0.1F; // to turn 0.029, row 5, over rows 1 to 4
    sweep.advance(frame, history);
    TEST_CHECK(sweep.armRow() == 5 && steppedAt(history, 5) == 4);
    frame.deltaSeconds = 0;
    sweep.advance(frame, history);
    TEST_CHECK(sweep.armRow() == 5 && steppedAt(history, 5) == 4);
    // After a reset the arm starts over on row 0, which a long first frame steps over too.
    sweep.reset();
    history.clear();
    frame.deltaSeconds = 0.1F;
    sweep.advance(frame, history);
    TEST_CHECK(sweep.armRow() == 4 && steppedAt(history, 4) == 4);
  }

  void hostileFramesAreBounded() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Hostile);
    for (int i = 0; i < 400; ++i) {
      sweep.advance(source.next(), history);
      TEST_CHECK(std::isfinite(sweep.turns()));
      TEST_CHECK(sweep.armRow() < kRadarBearings);
      TEST_CHECK(sweep.kick() >= 0 && sweep.kick() <= 1);
    }
    // No hostile frame steps more than kMaxAudioFrameSeconds.
    TEST_CHECK(sweep.turns() <= 400 * 0.1 * 0.25 + 1e-9);
  }

  void resetStartsOver() {
    RadarSweep sweep;
    auto history = radarHistory();
    AudioFrameSource source(AudioScene::Kicks);
    for (int i = 0; i < 70; ++i) {
      sweep.advance(source.next(), history);
    }
    sweep.reset();
    TEST_CHECK(sweep.turns() == 0 && sweep.armRow() == 0 && sweep.kick() == 0);
  }

  void refusesAnotherHistoryShape() {
    RadarSweep sweep;
    AudioHistory wrong(64, kRadarBearings);
    AudioFrameSource source(AudioScene::Tone);
    bool refused = false;
    try {
      sweep.advance(source.next(), wrong);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
  }
} // namespace

int main() {
  scopeFitsTheShorterSide();
  echoPutsLowNotesInside();
  echoOfBrokenBandsIsClamped();
  armTurnsAQuarterASecond();
  toneWritesAnArc();
  hitWritesASpoke();
  silenceWritesDarkRows();
  longestFrameStaysWithinReach();
  standingStillKeepsWhatWasSteppedOver();
  hostileFramesAreBounded();
  resetStartsOver();
  refusesAnotherHistoryShape();
  std::println("radar sweep checks passed");
  return 0;
}
