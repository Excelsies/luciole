#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <chrono>
#include <memory>
#include <print>
#include <thread>

// A drive is removed while its file plays: whenever the pipeline still holds the file, the player says a stop is
// pending, so the removal waits for it. Closing and destroying the player are both checked.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  // From the moment the player let go until the file is closed, the descriptor is never held while no stop is
  // pending. The rounds bound the wait.
  void neverHeldWithoutAPendingStop(const fs::path& clip) {
    bool sawHeld = false;
    for (int round = 0; round < 20'000; ++round) {
      const bool pending = MediaPlayer::stopsPending();
      const bool held = holdsOpen(clip);
      sawHeld = sawHeld || held;
      TEST_CHECK(!held || pending || MediaPlayer::stopsPending());
      if (!held && !pending) {
        std::println("  file held after the release: {}", sawHeld);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(false);
  }

  void opened(Pump& pump, MediaPlayer& player, const fs::path& clip) {
    TEST_CHECK(player.open(clip.string()));
    player.play();
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Playing && holdsOpen(clip); }));
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements({"playbin", "fakesink", "wavparse"}); !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("tone.wav");
  writeTone(clip, 4);
  Pump pump;
  {
    MediaPlayer player(AudioOutput::Silent);
    opened(pump, player, clip);
    TEST_CHECK(!MediaPlayer::stopsPending() || holdsOpen(clip));
    player.close();
    neverHeldWithoutAPendingStop(clip);
  }
  {
    auto player = std::make_unique<MediaPlayer>(AudioOutput::Silent);
    opened(pump, *player, clip);
    player.reset();
    neverHeldWithoutAPendingStop(clip);
  }
  std::println("media-drive-stop: all checks passed");
  return 0;
}
