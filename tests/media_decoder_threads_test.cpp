#include "media/decoder_threads.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>

// A software video decoder gets the threads its stream needs and no more: each extra frame thread holds a decoded
// frame and a codec context, which at 4K is tens of megabytes.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::media::decoderThreads;
  using namespace test::media;

  constexpr int kCores = 24;

  void countsThreadsFromThePixelRate() {
    TEST_CHECK(decoderThreads(160, 120, 10, 1, kCores) == 1);
    TEST_CHECK(decoderThreads(1920, 1080, 30, 1, kCores) == 2);
    TEST_CHECK(decoderThreads(1920, 1080, 60, 1, kCores) == 3);
    TEST_CHECK(decoderThreads(3840, 2160, 30, 1, kCores) == 5);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, kCores) == 9);
    // 29.97 and 59.94 frames a second are the same cost as 30 and 60.
    TEST_CHECK(decoderThreads(3840, 2160, 30000, 1001, kCores) == 5);
  }

  void boundsThreadsByTheCoresAndTheDecoderLimit() {
    TEST_CHECK(decoderThreads(7680, 4320, 60, 1, kCores) == 16);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, 4) == 4);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, 1) == 1);
  }

  void leavesUnknownStreamsToTheDecoder() {
    TEST_CHECK(decoderThreads(0, 0, 30, 1, kCores) == 0);
    // No frame rate: counted as 60.
    TEST_CHECK(decoderThreads(1920, 1080, 0, 0, kCores) == 3);
  }

  // The decoders' frame threads of this process, by their names.
  int frameThreads() {
    int found = 0;
    for (const auto& entry : fs::directory_iterator("/proc/self/task")) {
      std::string name;
      std::ifstream(entry.path() / "comm") >> name;
      found += name.starts_with("av:h264:df") ? 1 : 0;
    }
    return found;
  }

  bool writeH264(const fs::path& path, int width, int height, int fps, int frames) {
    return writeLaunched(
        std::format(
            "videotestsrc num-buffers={} pattern=ball ! video/x-raw,format=I420,width={},height={},framerate={}/1 ! "
            "x264enc speed-preset=ultrafast key-int-max={} ! h264parse ! mp4mux ! filesink location=\"{}\"",
            frames, width, height, fps, fps, path.string()
        )
    );
  }

  // Plays the clip until a frame arrives and counts the frame threads while it plays.
  int threadsWhilePlaying(Pump& pump, const fs::path& clip) {
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(clip.string()));
    player.play();
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Playing && player.takeFrame().has_value(); }));
    pump.run(std::chrono::milliseconds{300});
    return frameThreads();
  }

  void playsWithTheThreadsTheStreamNeeds(Pump& pump, Scratch& scratch) {
    struct Case {
      int width;
      int height;
      int fps;
      int threads;
    };
    for (const Case& item : {Case{1920, 1080, 30, 2}, Case{3840, 2160, 30, 5}, Case{1280, 720, 10, 0}}) {
      const auto clip = scratch.path(std::format("clip-{}.mp4", item.width).c_str());
      TEST_CHECK(writeH264(clip, item.width, item.height, item.fps, item.fps * 3));
      const int found = threadsWhilePlaying(pump, clip);
      std::println("{}x{} at {}: {} frame threads", item.width, item.height, item.fps, found);
      // One thread decodes on the streaming thread itself and has no frame threads of its own.
      TEST_CHECK(found == (item.threads > 1 ? item.threads : 0));
    }
  }
} // namespace

int main() {
  countsThreadsFromThePixelRate();
  boundsThreadsByTheCoresAndTheDecoderLimit();
  leavesUnknownStreamsToTheDecoder();
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements({"videotestsrc", "x264enc", "h264parse", "mp4mux", "filesink",
                                            "avdec_h264", "playbin", "appsink", "videoconvertscale", "fakesink"});
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  Pump pump;
  playsWithTheThreadsTheStreamNeeds(pump, scratch);
  return 0;
}
