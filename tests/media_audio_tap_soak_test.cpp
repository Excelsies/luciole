#include "media/media_player.h"
#include "media_clips.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <cmath>
#include <malloc.h>
#include <numbers>
#include <print>
#include <string>

// The tap over a long playback: copied once a frame, it takes no more memory at the end than near the start.
namespace {
  using luciole::MediaPlayer;
  using namespace test::media;
  using std::chrono::milliseconds;

  // What the heap has handed out and not had back, in every arena and in mapped blocks. Unlike resident pages, it
  // does not fall when freed heap goes back to the system, so it cannot hide growth.
  double allocatedMiB() {
    const struct mallinfo2 heap = ::mallinfo2();
    return static_cast<double>(heap.uordblks + heap.hblkhd) / (1024.0 * 1024.0);
  }

  // Playback is heard in real time, so the run is short: a leak of 17 KiB a second or more still shows in the limits.
  constexpr double kWarmSeconds = 5;
  constexpr double kMiddleSeconds = 15;
  constexpr double kEndSeconds = 25;

  void staysFlatWhilePlaying(Pump& pump, const fs::path& folder) {
    const auto clip = folder / "long.wav";
    writeWav(clip, 48000, 2, kEndSeconds + 2, [](int channel, double t) {
      return 0.4 * std::sin(2 * std::numbers::pi * (channel == 0 ? 220 : 330) * t);
    });
    const double before = allocatedMiB();
    Tapped tapped;
    TEST_CHECK(tapped.player.open(clip.string()));
    tapped.player.play();
    const auto playTo = [&](double seconds) {
      int fresh = 0;
      // A frame a round; the count bounds the loop at about twice the time asked for.
      for (int round = 0; round < 9000 && tapped.player.position() < seconds; ++round) {
        pump.run(milliseconds(16));
        fresh += tapped.copy().fresh ? 1 : 0;
      }
      TEST_CHECK(tapped.player.position() >= seconds);
      return fresh;
    };
    (void)playTo(kWarmSeconds);
    const double start = allocatedMiB();
    const double startProcessor = processorMs();
    int fresh = playTo(kMiddleSeconds);
    const double middle = allocatedMiB();
    fresh += playTo(kEndSeconds);
    const double busy = processorMs() - startProcessor;
    const double end = allocatedMiB();
    TEST_CHECK(tapped.copy().count == kTapLength);
    tapped.player.close();
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
    std::println(
        "allocated memory: {:.2f} MiB before, {:.2f} MiB at 5 s, {:.2f} MiB at 15 s, {:.2f} MiB at 25 s; {} fresh "
        "windows, {:.0f} ms of processor over 20 s",
        before, start, middle, end, fresh, busy
    );
    TEST_CHECK(end - start < 0.35);
    TEST_CHECK(end - middle < 0.2);
    TEST_CHECK(fresh > 20 * 30);
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements({"wavparse", "playbin", "appsink", "fakesink", "queue"}); !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  Pump pump;
  const auto folder = scratch.path("clips");
  fs::create_directory(folder);
  staysFlatWhilePlaying(pump, folder);
  std::println("media-audio-tap-soak: all checks passed");
  return 0;
}
