#include "media/media_player.h"
#include "media/pipeline_thread.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

// What playing a small clip costs in memory and processor time, and that both go back to rest after it stops.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  constexpr double kMiB = 1024.0 * 1024.0;

  double residentMiB() {
    std::ifstream statm("/proc/self/statm");
    long pages = 0;
    long resident = 0;
    statm >> pages >> resident;
    TEST_CHECK(statm.good() && resident > 0);
    return static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)) / kMiB;
  }

  // The resident anonymous memory: the heaps and stacks, without the pages of loaded libraries.
  double anonymousMiB() {
    std::ifstream rollup("/proc/self/smaps_rollup");
    std::string key;
    long kib = -1;
    // smaps_rollup has about twenty lines; the bound only guards against a format that never ends.
    for (int i = 0; i < 64 && rollup >> key; ++i) {
      if (key == "Anonymous:") {
        rollup >> kib;
        break;
      }
      rollup.ignore(256, '\n');
    }
    TEST_CHECK(kib > 0);
    return static_cast<double>(kib) / 1024.0;
  }

  struct Resting {
    double resident = 0;
    double anonymous = 0;
  };

  // The memory once the pipeline thread has freed a closed player, which a job queued behind it shows; the test
  // itself trims nothing.
  Resting resting(Pump& pump) {
    std::atomic<bool> drained{false};
    luciole::media::runOnPipelineThread([&drained] { drained = true; });
    TEST_CHECK(pump.until([&drained] { return drained.load(); }));
    pump.run(std::chrono::milliseconds(100));
    return {.resident = residentMiB(), .anonymous = anonymousMiB()};
  }

  // Processor milliseconds spent while the main context runs idle for half a second.
  double idleMs(Pump& pump) {
    const double start = processorMs();
    pump.run(std::chrono::milliseconds(500));
    return processorMs() - start;
  }

  struct Played {
    double peakMiB = 0;
    double busyMs = 0;  // processor time over the measured stretch
    double seconds = 0; // media time of that stretch
    int frames = 0;
  };

  // Plays `clip` the way a view does, taking each frame that arrives, and measures from 0.5 s to `until` seconds.
  Played play(Pump& pump, const fs::path& clip, double until = 2.5) {
    MediaPlayer player(AudioOutput::Silent);
    player.setFrameSize(320, 240);
    TEST_CHECK(player.open(clip.string()));
    player.play();
    Played played;
    const auto step = [&] {
      if (player.takeFrame()) {
        ++played.frames;
      }
      played.peakMiB = std::max(played.peakMiB, residentMiB());
    };
    TEST_CHECK(pump.until([&] {
      step();
      return player.position() >= 0.5;
    }));
    const double from = player.position();
    const double start = processorMs();
    TEST_CHECK(pump.until([&] {
      step();
      return player.position() >= until;
    }));
    played.busyMs = processorMs() - start;
    played.seconds = player.position() - from;
    TEST_CHECK(player.state() == MediaState::Playing);
    player.close();
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
    return played;
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "playbin", "appsink",
           "videoconvertscale", "fakesink"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  TEST_CHECK(writeClip(clip, {.width = 640, .height = 360, .fps = 30, .seconds = 4}));
  Pump pump;
  const auto cold = resting(pump);
  // The first run starts GStreamer and loads its plugins, which stay loaded; measure the second.
  const auto first = play(pump, clip);
  const auto before = resting(pump);
  const double restBefore = idleMs(pump);

  const auto played = play(pump, clip);
  const auto after = resting(pump);
  const double restAfter = idleMs(pump);
  // Ten more files in a row, each played for a moment and left, and ten after those.
  for (int i = 0; i < 10; ++i) {
    (void)play(pump, clip, 0.6);
  }
  const auto afterTen = resting(pump);
  for (int i = 0; i < 10; ++i) {
    (void)play(pump, clip, 0.6);
  }
  const auto afterTwenty = resting(pump);

  std::println(
      "resident before any play: {:.1f} MiB, peak while playing the first time: {:.1f} MiB, after it: {:.1f} MiB",
      cold.resident, first.peakMiB, before.resident
  );
  std::println(
      "resident before: {:.1f} MiB, peak while playing: {:.1f} MiB, after: {:.1f} MiB, after ten more: {:.1f} MiB",
      before.resident, played.peakMiB, after.resident, afterTen.resident
  );
  std::println(
      "anonymous before any play: {:.1f} MiB, after the first: {:.1f} MiB, after the second: {:.1f} MiB, "
      "after ten more: {:.1f} MiB, after ten more still: {:.1f} MiB",
      cold.anonymous, before.anonymous, after.anonymous, afterTen.anonymous, afterTwenty.anonymous
  );
  std::println(
      "processor while playing 640x360 at 30 fps into 320x240 frames: {:.0f} ms over {:.2f} s ({:.1f}%), "
      "{} frames taken",
      played.busyMs, played.seconds, played.busyMs / (played.seconds * 10.0), played.frames
  );
  std::println("processor at rest: {:.1f} ms before, {:.1f} ms after, over half a second", restBefore, restAfter);
  TEST_CHECK(played.seconds > 1.5);
  TEST_CHECK(played.frames >= 30);
  TEST_CHECK(played.peakMiB >= before.resident);
  // Back to rest: the heap the playback took is returned and no thread keeps working. The streaming threads free
  // into heaps of their own; left untrimmed, a second play keeps about 7 MiB of them. Where the first play leaves
  // the resting heap varies by a few MiB with the timing of the frees, so the files after the first ten are held to
  // the first ten.
  TEST_CHECK(after.anonymous - before.anonymous < 5.0);
  TEST_CHECK(afterTwenty.anonymous - afterTen.anonymous < 3.0);
  TEST_CHECK(restAfter < 20.0);
  std::println("media-player-load: all checks passed");
  return 0;
}
