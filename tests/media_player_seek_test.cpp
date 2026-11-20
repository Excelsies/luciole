#include "media/media_player.h"
#include "media_clips.h"
#include "media_loop_rig.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <print>
#include <thread>

// Seeking: a seek in flight, the ones that wait behind it, and how they meet the end of the file. The tests that need
// to see which seeks reach the pipeline play through the recording sound output of media_loop_rig.h.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::SeekMode;
  using namespace test::media;

  using namespace std::chrono_literals;

  // The end arrives while the main context is not looking; a seek made then is not undone by that end.
  void aQueuedEndDoesNotOverrideASeek(Pump& pump, const Clips& clips, bool loop) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, loop);
    std::this_thread::sleep_for(1300ms);       // the file ends, its message waits unheard
    TEST_NEAR(player.position(), kTone, 0.05); // the end was reached, so its message is queued
    player.seek(0.4);
    pump.run(50ms);
    TEST_CHECK(player.state() == MediaState::Playing && !watched.ended);
    TEST_CHECK(player.position() > 0.35 && player.position() < 0.75);
    // The end the seek leads to still arrives and is handled.
    if (loop) {
      TEST_CHECK(watched.passed(pump, 1));
      TEST_CHECK(player.state() == MediaState::Playing);
    } else {
      TEST_CHECK(reached(pump, player, MediaState::Ended));
    }
  }

  // An end waits unheard, then a seek to the very end: the stale end is kept while the seek is in flight, the seek
  // settles at the end and the file ends or restarts once, and the seek's own end that follows does nothing more.
  // endMissed: the stale end held until the seek settles is covered here; the seek's own end arriving before its done
  // message is not, since the pipeline posts the done message first and that order cannot be forced.
  void aQueuedEndThenASeekToTheEndHandlesTheEndOnce(Pump& pump, const Clips& clips, bool loop) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, loop);
    std::this_thread::sleep_for(1300ms);
    TEST_NEAR(player.position(), kTone, 0.05);
    player.seek(kTone);
    if (loop) {
      TEST_CHECK(watched.passed(pump, 1));
      pump.run(300ms); // the seek's own end arrives and restarts nothing more
      TEST_CHECK(watched.passes == 1 && !watched.ended && player.state() == MediaState::Playing);
    } else {
      TEST_CHECK(reached(pump, player, MediaState::Ended));
      pump.run(300ms);
      TEST_CHECK(player.state() == MediaState::Ended && watched.endedNotices == 1);
    }
  }

  // Two seeks one after the other, the first to the end: its end must not end the file after the second moved away.
  // Each round proves the first seek reached the output before the second was asked, and at least kMostlySent of the
  // rounds must.
  void aSecondSeekAwayFromTheEndKeepsPlaying(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    constexpr int kRounds = 10;
    constexpr int kMostlySent = 5;
    int sent = 0;
    for (int round = 0; round < kRounds; ++round) {
      Watched watched(AudioOutput::System);
      auto& player = watched.player;
      playTone(pump, watched, clips, false);
      TEST_CHECK(pump.until([&] { return player.position() > 0.2; }));
      const int before = output().seeks.load();
      player.seek(kTone);
      const bool reachedOutput = pump.until([&] { return output().seeks.load() > before; }, 500ms);
      sent += reachedOutput ? 1 : 0;
      player.seek(0.3);
      pump.run(250ms);
      TEST_CHECK(player.state() == MediaState::Playing && !watched.ended);
      TEST_CHECK(player.position() > 0.35 && player.position() < 0.75);
    }
    std::println("the first seek was in flight when the second was asked in {} of {} rounds", sent, kRounds);
    TEST_CHECK(sent >= kMostlySent);
  }

  // A seek to the very end gives a fresh end, which ends the file or loops it.
  void aSeekToTheVeryEndStillEnds(Pump& pump, const Clips& clips, bool loop) {
    Watched watched;
    auto& player = watched.player;
    playTone(pump, watched, clips, loop);
    TEST_CHECK(pump.until([&] { return player.position() > 0.2; }));
    player.seek(kTone);
    if (loop) {
      TEST_CHECK(watched.passed(pump, 1));
      TEST_CHECK(player.state() == MediaState::Playing);
    } else {
      TEST_CHECK(reached(pump, player, MediaState::Ended));
    }
  }

  // The frame size changes while paused: the pipeline asks for the shown frame again at the new size. Seeks asked
  // while that is in flight wait behind it and only the last goes out, so the output sees two seeks in all.
  void aRefitDoesNotLetASeekOvertakeIt(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(player.hasVideo());
    pump.run(300ms);            // the first frame is decoded and nothing else is asked
    output().seekDelayMs = 600; // far above the main thread's worst stall, so the seeks below meet it in flight
    const int before = output().seeks.load();
    player.setFrameSize(80, 60);
    TEST_CHECK(pump.until([&] { return output().seeks.load() > before; })); // the refit is in flight
    player.seek(0.5);
    player.seek(1.0);
    player.seek(1.5);
    TEST_CHECK(pump.until([&] { return output().seeks.load() >= before + 2; }));
    output().seekDelayMs = 0;
    TEST_CHECK(pump.until([&] {
      return std::abs(player.position() - 1.5) < 0.15 && player.state() == MediaState::Paused;
    }));
    pump.run(300ms); // a third seek would reach the output by now
    std::println("seeks that reached the output: {}", output().seeks.load() - before);
    TEST_CHECK(output().seeks.load() == before + 2);
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), 1.5, 0.15);
  }

  // The position and the time of the last notice the player gave.
  struct Timed {
    MediaPlayer player{AudioOutput::Silent};
    Clock::time_point lastNotice = Clock::now();
    double positionAtNotice = 0;

    explicit Timed(AudioOutput output = AudioOutput::Silent) : player(output) {
      player.setChangedCallback([this] {
        lastNotice = Clock::now();
        positionAtNotice = player.position();
      });
    }
    // Pumps until nothing was heard for `quiet`, and at least one notice came after `since`.
    bool settled(Pump& pump, Clock::time_point since, std::chrono::milliseconds quiet = 300ms) {
      return pump.until([&] { return lastNotice > since && Clock::now() - lastNotice > quiet; });
    }
  };

  struct Dragged {
    double latencyMs = 0;
    double settledAt = 0;
    int requested = 0;
    int seen = 0;
    double backward = 0; // the largest step back between two reads during the drag
    MediaState state = MediaState::Idle;
  };

  // A playing video's position may read a few milliseconds behind the target it was just moved to (4 ms at most in 12
  // runs); 20 ms, about an audio buffer, is far below anything a position bar shows.
  constexpr double kBackStep = 0.02;
  constexpr int kDragSteps = 30;
  constexpr double kDragStart = 0.4;
  constexpr double kDragStep = 0.04;

  // 30 key frame seeks about 10 ms apart, each above the one before, then the release as one exact seek to the last
  // target. With `playing`, the file plays throughout.
  Dragged drag(Pump& pump, const fs::path& clip, bool playing, AudioOutput audio) {
    Timed timed(audio);
    auto& player = timed.player;
    TEST_CHECK(player.open(clip.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    if (playing) {
      player.play();
      TEST_CHECK(pump.until([&] { return player.position() > 0.1; }));
    }
    Dragged result;
    const int before = output().seeks.load();
    double previous = player.position();
    for (int i = 0; i < kDragSteps; ++i) {
      player.seek(kDragStart + (i * kDragStep), SeekMode::KeyFrame);
      ++result.requested;
      pump.run(10ms);
      const double now = player.position();
      result.backward = std::max(result.backward, previous - now);
      previous = now;
    }
    const double target = kDragStart + ((kDragSteps - 1) * kDragStep);
    const auto released = Clock::now();
    player.seek(target, SeekMode::Exact);
    ++result.requested;
    TEST_CHECK(timed.settled(pump, released));
    result.latencyMs = std::chrono::duration<double, std::milli>(timed.lastNotice - released).count();
    result.settledAt = timed.positionAtNotice - target;
    result.seen = output().seeks.load() - before;
    result.state = player.state();
    return result;
  }

  void aDragThenAReleaseSettlesAtTheLastTarget(Pump& pump, const Clips& clips, bool video, bool playing) {
    const auto& clip = video ? clips.video : clips.sound;
    const double tolerance = video ? 0.11 : 0.06; // a frame interval, or about a tenth of a second of sound
    const auto result = drag(pump, clip, playing, video ? AudioOutput::Silent : AudioOutput::System);
    std::println(
        "{} {}: {} seeks asked, {} reached the pipeline, settled {:+.4f} s from the target, release took {:.2f} ms, "
        "position stepped back by up to {:.6f} s",
        video ? "video" : "sound", playing ? "playing" : "paused", result.requested, result.seen, result.settledAt,
        result.latencyMs, result.backward
    );
    TEST_CHECK(result.state == (playing ? MediaState::Playing : MediaState::Paused));
    TEST_CHECK(std::abs(result.settledAt) <= tolerance + (playing ? 0.1 : 0.0));
    TEST_CHECK(result.backward <= kBackStep);
    TEST_CHECK(result.seen >= 2 && result.seen <= result.requested);
  }

  // A seek and a second one wait; the file is closed and another opened: the other file starts paused at its start
  // and no seek reaches it that it was not asked for.
  void aWaitingSeekDoesNotReachAnotherFile(Pump& pump, const Clips& clips) {
    useRecordingOutput();
    Watched watched(AudioOutput::System);
    auto& player = watched.player;
    TEST_CHECK(player.open(clips.sound.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.seek(1.0);
    player.seek(2.0); // waits behind the first
    player.close();
    drainPipelineThread(pump); // the first file's requests have all run
    const int before = output().seeks.load();
    TEST_CHECK(player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    pump.run(500ms);
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), 0.0, 1e-9);
    TEST_NEAR(player.duration(), kTone, 0.05);
    TEST_CHECK(output().seeks.load() == before);
  }

  // A seek and a second one wait while the file is paused, then played: it stands at the last target, paused, and
  // plays on from there.
  void aWaitingSeekAcrossPauseAndPlay(Pump& pump, const Clips& clips) {
    Timed timed;
    auto& player = timed.player;
    TEST_CHECK(player.open(clips.sound.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() > 0.1; }));
    const auto asked = Clock::now();
    player.seek(1.0);
    player.seek(2.0); // waits behind the first
    player.pause();
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_CHECK(timed.settled(pump, asked));
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), 2.0, 0.05);
    pump.run(400ms);
    TEST_NEAR(player.position(), 2.0, 0.05); // paused: it did not move on
    player.play();
    TEST_CHECK(player.state() == MediaState::Playing);
    double lowest = player.position();
    TEST_CHECK(pump.until([&] {
      lowest = std::min(lowest, player.position());
      return player.position() > 2.15;
    }));
    TEST_CHECK(lowest > 1.95 && player.state() == MediaState::Playing);
  }

  // A seek to the very end of a file, playing: the file ends or, with loop on, restarts once and plays on.
  void aSeekToTheEndOfTheFile(Pump& pump, const fs::path& clip, AudioOutput audio, bool loop) {
    Watched watched(audio);
    auto& player = watched.player;
    player.setLoop(loop);
    TEST_CHECK(player.open(clip.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() > 0.2; }));
    const int before = output().seeks.load();
    player.seek(player.duration());
    if (loop) {
      TEST_CHECK(pump.until([&] { return output().seeks.load() >= before + 2; })); // the seek and the restart
      pump.run(500ms);
      TEST_CHECK(output().seeks.load() == before + 2);
      TEST_CHECK(!watched.ended && player.state() == MediaState::Playing && player.position() < 1.5);
    } else {
      TEST_CHECK(reached(pump, player, MediaState::Ended));
      pump.run(300ms);
      TEST_CHECK(player.state() == MediaState::Ended && watched.endedNotices == 1);
    }
  }

  // The end waits unheard while a seek and a second one wait for it, and the pipeline refuses both: the file is at
  // its end all the same. With loop off it ends; with loop on it starts again, since the restart is not refused.
  void aLostEndSurvivesTwoRefusedSeeks(Pump& pump, const Clips& clips, bool loop) {
    useRecordingOutput();
    Watched watched(AudioOutput::System);
    auto& player = watched.player;
    playTone(pump, watched, clips, loop);
    TEST_CHECK(pump.until([&] { return player.position() > 0.2; })); // earlier messages are heard
    std::this_thread::sleep_for(1300ms);                             // the end waits alone
    const int before = output().seeks.load();
    output().refuseNext = 2;
    player.seek(0.3);
    player.seek(0.4); // waits behind the first
    if (loop) {
      TEST_CHECK(pump.until([&] { return output().seeks.load() >= before + 3; })); // the restart reaches the output
      pump.run(100ms);
      TEST_CHECK(!watched.ended && player.state() == MediaState::Playing);
      TEST_CHECK(output().seeks.load() == before + 3 && player.position() < kNearStart);
    } else {
      TEST_CHECK(reached(pump, player, MediaState::Ended));
      pump.run(300ms);
      TEST_CHECK(player.state() == MediaState::Ended && watched.endedNotices == 1);
      TEST_CHECK(output().seeks.load() == before + 2);
    }
    output().refuseNext = 0;
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("media-player-seek: playback not built");
    return 0;
  }
  Scratch scratch;
  const auto missing = missingElements(
      {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "playbin", "appsink",
       "videoconvertscale", "fakesink", "identity"}
  );
  if (!missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  Clips clips;
  clips.tone = scratch.path("tone.wav");
  clips.sound = scratch.path("sound.wav");
  clips.video = scratch.path("clip.avi");
  clips.picture = scratch.path("picture.avi");
  const auto sine = [](int /*channel*/, double at) { return 0.6 * std::sin(2.0 * std::numbers::pi * 440.0 * at); };
  writeWav(clips.tone, kTapRate, 1, kTone, sine);
  writeWav(clips.sound, kTapRate, 1, 3.0, sine);
  TEST_CHECK(writeClip(clips.video, {}));
  TEST_CHECK(writeClip(clips.picture, {.audio = false}));
  Pump pump;
  countVideoSeeks();
  aQueuedEndDoesNotOverrideASeek(pump, clips, false);
  aQueuedEndDoesNotOverrideASeek(pump, clips, true);
  aQueuedEndThenASeekToTheEndHandlesTheEndOnce(pump, clips, false);
  aQueuedEndThenASeekToTheEndHandlesTheEndOnce(pump, clips, true);
  aSecondSeekAwayFromTheEndKeepsPlaying(pump, clips);
  aSeekToTheVeryEndStillEnds(pump, clips, false);
  aSeekToTheVeryEndStillEnds(pump, clips, true);
  aLostEndSurvivesTwoRefusedSeeks(pump, clips, false);
  aLostEndSurvivesTwoRefusedSeeks(pump, clips, true);
  aRefitDoesNotLetASeekOvertakeIt(pump, clips);
  useRecordingOutput();
  for (const bool video : {false, true}) {
    for (const bool playing : {false, true}) {
      aDragThenAReleaseSettlesAtTheLastTarget(pump, clips, video, playing);
    }
  }
  for (const bool loop : {false, true}) {
    aSeekToTheEndOfTheFile(pump, clips.sound, AudioOutput::System, loop);
    aSeekToTheEndOfTheFile(pump, clips.video, AudioOutput::Silent, loop);
    aSeekToTheEndOfTheFile(pump, clips.picture, AudioOutput::Silent, loop);
  }
  aWaitingSeekDoesNotReachAnotherFile(pump, clips);
  aWaitingSeekAcrossPauseAndPlay(pump, clips);
  std::println("media-player-seek: all checks passed");
  return 0;
}
