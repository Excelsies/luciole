#include "media/media_player.h"
#include "media_clips.h"
#include "media_loop_rig.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <print>
#include <thread>

// A paused video whose frame size changes shows its frame decoded again at the new size: after a resize and a pause
// together, in a file that has ended, while loading, and when the pipeline refuses the seek that does it.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  using namespace std::chrono_literals;

  constexpr auto kFrameWait = 5000ms;
  constexpr int kFullWidth = 160; // the clip's own size
  constexpr int kBoxWidth = 80;   // setFrameSize(80, 80) fits the 160 by 120 clip to 80 by 60

  // The width of the newest frame the player handed out.
  struct Shown {
    int width = 0;
    void take(MediaPlayer& player) {
      if (const auto frame = player.takeFrame()) {
        width = frame->width;
      }
    }
    bool reaches(Pump& pump, MediaPlayer& player, int wanted, std::chrono::milliseconds limit = kFrameWait) {
      return pump.until(
          [&] {
            take(player);
            return width == wanted;
          },
          limit
      );
    }
  };

  // A resize and a pause in one turn of the main loop: the pipeline sees the pause before it has asked for it.
  void aResizeAndAPauseTogetherRefitTheFrame(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    Shown shown;
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() > 0.3; }));
    shown.take(player);
    TEST_CHECK(shown.width == kFullWidth);
    const int before = output().seeks.load();
    player.setFrameSize(kBoxWidth, kBoxWidth);
    player.pause();
    TEST_CHECK(shown.reaches(pump, player, kBoxWidth));
    pump.run(400ms);
    std::println("seeks after a resize and a pause together: {}", output().seeks.load() - before);
    TEST_CHECK(output().seeks.load() == before + 1);
    TEST_CHECK(player.state() == MediaState::Paused && !watched.failed);
  }

  // An ended video, resized: the last frame comes again at the new size, the file stays ended, listeners hear no
  // second end and a loop that was switched on does not start it again.
  void aResizeAfterTheEndShowsTheLastFrame(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    Shown shown;
    TEST_CHECK(player.open(clips.video.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    player.play();
    TEST_CHECK(reached(pump, player, MediaState::Ended));
    pump.run(300ms);
    shown.take(player);
    TEST_CHECK(shown.width == kFullWidth);
    player.setLoop(true);
    const int notices = watched.endedNotices;
    const int before = output().seeks.load();
    player.setFrameSize(kBoxWidth, kBoxWidth);
    TEST_CHECK(shown.reaches(pump, player, kBoxWidth));
    pump.run(500ms);
    TEST_CHECK(player.state() == MediaState::Ended);
    TEST_CHECK(watched.endedNotices == notices);
    TEST_CHECK(output().seeks.load() == before + 1);
    TEST_NEAR(player.position(), player.duration(), 1e-9);
    player.play();
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Playing && player.position() < 1.0; }));
  }

  // A resize that lands while the file is still loading: the frame is redrawn once the file is paused.
  void aResizeWhileLoadingIsRefitOncePaused(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    Shown shown;
    TEST_CHECK(player.open(clips.video.string()));
    std::this_thread::sleep_for(600ms); // the file is decoded and its done message waits unheard
    player.setFrameSize(kBoxWidth, kBoxWidth);
    std::this_thread::sleep_for(200ms);
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(shown.reaches(pump, player, kBoxWidth));
    pump.run(300ms);
    TEST_CHECK(player.state() == MediaState::Paused && !watched.failed);
  }

  // The pipeline refuses the refit seek: the file stays paused with its old frame, and later seeks go through. The
  // picture has no sound, so the video branch is the only one asked.
  void aRefusedRefitLeavesThePlayerUsable(Pump& pump, const Clips& clips) {
    Watched watched;
    auto& player = watched.player;
    Shown shown;
    TEST_CHECK(player.open(clips.picture.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(pump.until([&] {
      shown.take(player);
      return shown.width == kFullWidth;
    }));
    const int before = output().seeks.load();
    output().refuseVideoNext = 1;
    player.setFrameSize(kBoxWidth, kBoxWidth);
    TEST_CHECK(pump.until([&] { return output().seeks.load() > before; }));
    TEST_CHECK(!shown.reaches(pump, player, kBoxWidth, 500ms));
    TEST_CHECK(player.state() == MediaState::Paused && !watched.failed);
    output().refuseVideoNext = 0;
    player.seek(1.5);
    TEST_CHECK(shown.reaches(pump, player, kBoxWidth));
    TEST_CHECK(player.state() == MediaState::Paused);
    TEST_NEAR(player.position(), 1.5, 0.15);
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("media-player-refit: playback not built");
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
  clips.video = scratch.path("clip.avi");
  clips.picture = scratch.path("picture.avi");
  TEST_CHECK(writeClip(clips.video, {}));
  TEST_CHECK(writeClip(clips.picture, {.audio = false}));
  Pump pump;
  countVideoSeeks();
  aResizeAndAPauseTogetherRefitTheFrame(pump, clips);
  aResizeAfterTheEndShowsTheLastFrame(pump, clips);
  aResizeWhileLoadingIsRefitOncePaused(pump, clips);
  aRefusedRefitLeavesThePlayerUsable(pump, clips);
  std::println("media-player-refit: all checks passed");
  return 0;
}
