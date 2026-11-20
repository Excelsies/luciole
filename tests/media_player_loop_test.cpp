#include "media/media_player.h"
#include "media_clips.h"
#include "media_loop_rig.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <print>
#include <vector>

// Looping: a file that reaches its end plays again from the start, and the bounds that keep it from spinning. The
// player plays through the silent sound output, except in the tests that need to see when sound reaches the output:
// those use a sink that plays nothing and records when each buffer was due.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  constexpr std::size_t kMostRestarts = 5; // of the 20 ms clip, however late each pass ends

  // The gaps between the end of the last buffer of a pass and the first of the next, in milliseconds.
  std::vector<double> restartGaps() {
    const std::scoped_lock lock(output().lock);
    const auto& buffers = output().buffers;
    std::vector<double> gaps;
    for (std::size_t i = 1; i < buffers.size(); ++i) {
      if (buffers[i].pts <= buffers[i - 1].pts) {
        const auto ended = buffers[i - 1].at + std::chrono::nanoseconds(buffers[i - 1].duration);
        gaps.push_back(std::chrono::duration<double, std::milli>(buffers[i].at - ended).count());
      }
    }
    return gaps;
  }

  void startsOffAndStaysAcrossOpen(const Clips& clips) {
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(!player.loop());
    player.setLoop(true);
    TEST_CHECK(player.loop());
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(player.loop());
    TEST_CHECK(player.open(clips.tiny.string()));
    TEST_CHECK(player.loop());
    player.close();
    TEST_CHECK(player.loop());
    player.setLoop(false);
    TEST_CHECK(!player.loop());
  }

  // With loop on, the file passes its end twice and plays on: never Ended, and the position returns near zero.
  void playsPastItsEndAgainAndAgain(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, true);
    TEST_CHECK(watched.passed(pump, 2));
    TEST_CHECK(player.state() == MediaState::Playing);
    TEST_CHECK(!watched.ended && !watched.failed);
    TEST_CHECK(player.position() < kTone);
    TEST_CHECK(player.error().empty());
  }

  void endsAsBeforeWithLoopOff(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    TEST_NEAR(player.position(), kTone, 0.11);
    TEST_CHECK(watched.passes == 0);
    pump.run(std::chrono::milliseconds(300));
    TEST_CHECK(player.state() == MediaState::Ended);
  }

  void turningLoopOffLetsItEnd(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, true);
    TEST_CHECK(watched.passed(pump, 1));
    player.setLoop(false);
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    TEST_CHECK(watched.passes == 1);
  }

  void turningLoopOnWhilePlayingLoopsAtTheNextEnd(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() > 0.2; }));
    player.setLoop(true);
    TEST_CHECK(watched.passed(pump, 1));
    TEST_CHECK(!watched.ended && player.state() == MediaState::Playing);
  }

  // Paused stays paused, at the place it stopped, for longer than the file takes.
  void pausedStaysPaused(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    player.setLoop(true);
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    pump.run(std::chrono::milliseconds(300));
    TEST_CHECK(player.state() == MediaState::Paused);
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() > 0.2; }));
    player.pause();
    pump.run(std::chrono::milliseconds(100));
    const double held = player.position();
    pump.run(std::chrono::milliseconds(1200));
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), held, 0.05);
    TEST_CHECK(!watched.ended && watched.passes == 0);
  }

  // A seek and a Play after the file came round count as playing forward again.
  void aSeekToTheEndLoopsAgain(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, true);
    TEST_CHECK(watched.passed(pump, 1));
    // Right after a restart, a seek close to the end reaches the end well inside the minimum pass.
    player.seek(kTone - 0.03);
    TEST_CHECK(watched.passed(pump, 2));
    TEST_CHECK(!watched.ended && player.state() == MediaState::Playing);
  }

  // No spin: a file that ends again within kMinLoopPass of its restart ends. The 20 ms clip plays twice.
  void aPassShorterThanTheMinimumEnds(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    forgetBuffers();
    MediaPlayer player(AudioOutput::System);
    player.setLoop(true);
    TEST_CHECK(player.open(clips.tiny.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    pump.run(std::chrono::milliseconds(500)); // a restart that came late would show now
    TEST_CHECK(player.state() == MediaState::Ended);
    const auto restarts = restartGaps().size();
    std::println("a 20 ms clip with loop on restarted {} time(s) before it ended", restarts);
    // It restarts at least once; the minimum pass bounds the count when a restart's pass runs long on a loaded machine.
    TEST_CHECK(restarts >= 1 && restarts <= kMostRestarts);
  }

  // A file whose length is not known is not restarted: the output sees no seek, as it would for each restart.
  void aFileWithoutLengthEnds(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    output().seeks = 0;
    MediaPlayer player(AudioOutput::System);
    player.setLoop(true);
    TEST_CHECK(player.open(clips.empty.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_NEAR(player.duration(), 0.0, 1e-9);
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    pump.run(std::chrono::milliseconds(300));
    TEST_CHECK(player.state() == MediaState::Ended);
    TEST_CHECK(output().seeks.load() == 0);
  }

  // A restart the pipeline refuses ends the file the normal way, and the same file loops when it is not refused.
  void aRefusedRestartEnds(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    {
      MediaPlayer player(AudioOutput::System);
      player.setLoop(true);
      TEST_CHECK(player.open(clips.tone.string()));
      TEST_CHECK(reached(pump, player, MediaState::Paused));
      output().refuseSeeks = true;
      player.play();
      TEST_CHECK(reached(pump, player, MediaState::Ended));
      TEST_CHECK(player.error().empty());
      pump.run(std::chrono::milliseconds(300));
      TEST_CHECK(player.state() == MediaState::Ended);
      output().refuseSeeks = false;
      // Play from Ended asks for the start again, which the output takes.
      player.play();
      TEST_CHECK(pump.until([&] { return player.state() == MediaState::Playing && player.position() < kNearStart; }));
    }
    output().refuseSeeks = false;
  }

  // The sound is heard again within a short gap after the end, and the picture keeps no pause either.
  void measuresTheGapAtTheRestart(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    forgetBuffers();
    MediaPlayer player(AudioOutput::System);
    player.setLoop(true);
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return restartGaps().size() >= 3; }));
    auto gaps = restartGaps();
    std::ranges::sort(gaps);
    std::println(
        "gap between the last sound before the end and the first after it: median {:.1f} ms, max {:.1f} ms",
        gaps[gaps.size() / 2], gaps.back()
    );
    TEST_CHECK(gaps.front() > -60);
    TEST_CHECK(gaps[gaps.size() / 2] < 150);
  }

  // The tap hears the tone again after the restart.
  void theTapDeliversFreshSoundAfterARestart(Pump& pump, const Clips& clips) {
    Tapped tapped;
    auto& player = tapped.player;
    player.setLoop(true);
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(tapped.full(pump));
    bool late = false;
    int wraps = 0;
    TEST_CHECK(pump.until([&] {
      const double at = player.position();
      if (at > kTone * 0.5) {
        late = true;
      } else if (late && at < kNearStart) {
        late = false;
        ++wraps;
      }
      return wraps >= 1;
    }));
    TEST_CHECK(player.state() == MediaState::Playing);
    // Fresh windows full of the tone arrive after the wrap, and the position keeps moving forward.
    const double after = player.position();
    TEST_CHECK(tapped.full(pump));
    double peak = 0;
    for (const float value : tapped.valid()) {
      peak = std::max(peak, static_cast<double>(std::abs(value)));
    }
    TEST_CHECK(peak > 0.2);
    TEST_CHECK(pump.until([&] { return player.position() > after + 0.1 || player.position() < after; }));
    TEST_CHECK(player.state() == MediaState::Playing);
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    MediaPlayer player;
    player.setLoop(true);
    TEST_CHECK(player.loop());
    std::println("media-player-loop: playback not built; the switch is kept");
    return 0;
  }
  Scratch scratch;
  const auto missing = missingElements({"playbin", "wavparse", "appsink", "fakesink", "identity"});
  if (!missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  Clips clips;
  clips.tone = scratch.path("tone.wav");
  clips.tiny = scratch.path("tiny.wav");
  clips.empty = scratch.path("empty.wav");
  const auto sine = [](int /*channel*/, double at) { return 0.6 * std::sin(2.0 * std::numbers::pi * 440.0 * at); };
  writeWav(clips.tone, kTapRate, 1, kTone, sine);
  writeWav(clips.tiny, kTapRate, 1, 0.02, sine);
  writeWav(clips.empty, kTapRate, 1, 0.0, sine);
  Pump pump;
  startsOffAndStaysAcrossOpen(clips);
  playsPastItsEndAgainAndAgain(pump, clips);
  endsAsBeforeWithLoopOff(pump, clips);
  turningLoopOffLetsItEnd(pump, clips);
  turningLoopOnWhilePlayingLoopsAtTheNextEnd(pump, clips);
  pausedStaysPaused(pump, clips);
  aSeekToTheEndLoopsAgain(pump, clips);
  aFileWithoutLengthEnds(pump, clips);
  theTapDeliversFreshSoundAfterARestart(pump, clips);
  aPassShorterThanTheMinimumEnds(pump, clips);
  aRefusedRestartEnds(pump, clips);
  measuresTheGapAtTheRestart(pump, clips);
  std::println("media-player-loop: all checks passed");
  return 0;
}
