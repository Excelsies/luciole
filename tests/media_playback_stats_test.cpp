#include "media/media_player.h"
#include "media/pipeline_thread.h"
#include "media/playback_stats.h"
#include "media_clips.h"
#include "test_check.h"

#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <unistd.h>

// The picture counts a benchmark reads from the trace line of a finished playback.
namespace {
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::media::missedFrames;
  using namespace test::media;

  constexpr std::int64_t kFrame = 33'333'333; // 30 frames a second

  void countsTheFramesLostBetweenTwoPictures() {
    TEST_CHECK(missedFrames(0, kFrame, kFrame) == 0);
    // One picture of jitter either way is not a loss.
    TEST_CHECK(missedFrames(0, kFrame + kFrame / 3, kFrame) == 0);
    TEST_CHECK(missedFrames(0, 2 * kFrame, kFrame) == 1);
    TEST_CHECK(missedFrames(kFrame, 5 * kFrame, kFrame) == 3);
    // Nothing is known, or the time goes back, as after a seek: nothing is counted.
    TEST_CHECK(missedFrames(-1, 5 * kFrame, kFrame) == 0);
    TEST_CHECK(missedFrames(5 * kFrame, kFrame, kFrame) == 0);
    TEST_CHECK(missedFrames(0, 5 * kFrame, 0) == 0);
  }

  // Everything the process writes to stderr while `run` runs.
  template <typename Run> std::string capturedStderr(const std::string& file, Run run) {
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    const int target = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    TEST_CHECK(saved >= 0 && target >= 0 && ::dup2(target, STDERR_FILENO) >= 0);
    ::close(target);
    run();
    std::fflush(stderr);
    TEST_CHECK(::dup2(saved, STDERR_FILENO) >= 0);
    ::close(saved);
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  std::uint64_t field(const std::string& text, const std::string& name) {
    const auto at = text.find(name + "=");
    TEST_CHECK(at != std::string::npos);
    return std::stoull(text.substr(at + name.size() + 1));
  }

  // A clip played to its end delivers every picture and loses none; the line comes once, when the player closes.
  void aFinishedPlaybackPrintsItsCounts(Pump& pump, const fs::path& clip, const std::string& file) {
    ::setenv("LUCIOLE_STARTUP_TRACE", "1", 1);
    const auto text = capturedStderr(file, [&] {
      MediaPlayer player(luciole::AudioOutput::Silent);
      TEST_CHECK(player.open(clip.string()));
      player.play();
      std::uint64_t seen = 0;
      TEST_CHECK(pump.until([&] {
        seen += player.takeFrame().has_value() ? 1 : 0;
        return player.state() == MediaState::Ended;
      }));
      player.close();
      TEST_CHECK(pump.until([] { return !MediaPlayer::stopsPending(); }));
    });
    ::unsetenv("LUCIOLE_STARTUP_TRACE");
    TEST_CHECK(text.find("playback delivered=") != std::string::npos);
    std::println("trace: {}", text.substr(text.find("luciole-trace")));
    // The 30 pictures of the clip; the first comes as the preroll and again as a sample, and counts once.
    TEST_CHECK(field(text, "delivered") == 30);
    TEST_CHECK(field(text, "missed") == 0);
    // The view may take the preroll picture and then the same picture again as a sample.
    TEST_CHECK(field(text, "taken") + field(text, "replaced") <= 31);
  }

  void saysNothingWithoutTheTrace(Pump& pump, const fs::path& clip, const std::string& file) {
    ::unsetenv("LUCIOLE_STARTUP_TRACE");
    const auto text = capturedStderr(file, [&] {
      MediaPlayer player(luciole::AudioOutput::Silent);
      TEST_CHECK(player.open(clip.string()));
      player.play();
      TEST_CHECK(pump.until([&] { return player.state() == MediaState::Ended; }));
      player.close();
      TEST_CHECK(pump.until([] { return !MediaPlayer::stopsPending(); }));
    });
    TEST_CHECK(text.find("playback") == std::string::npos);
  }
} // namespace

int main() {
  countsTheFramesLostBetweenTwoPictures();
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements({"videotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "playbin",
                                            "appsink", "videoconvertscale", "fakesink"});
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .fps = 10, .seconds = 3, .audio = false}));
  Pump pump;
  aFinishedPlaybackPrintsItsCounts(pump, clip, scratch.path("stderr.txt").string());
  saysNothingWithoutTheTrace(pump, clip, scratch.path("stderr2.txt").string());
  std::println("media-playback-stats: all checks passed");
  return 0;
}
