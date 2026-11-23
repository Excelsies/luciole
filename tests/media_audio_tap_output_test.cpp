#include "cpu_budget.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <print>
#include <vector>

// The tap leaves the sound alone: the sink that stands in for the device receives the same bytes, without gaps, with
// the tap off, on, and switched on and off while playing.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  using Clock = std::chrono::steady_clock;
  using std::chrono::milliseconds;

  struct Recording {
    std::size_t size = 0;
    std::uint64_t hash = 0;
    int gaps = 0;
    int windows = 0; // copies that held samples
    double busyMs = 0;
  };

  // Plays `clip` to its end through the late sink, calling `during` each round, and returns what the sink played.
  Recording playThrough(Pump& pump, const fs::path& clip, const std::function<void(MediaPlayer&, int)>& during) {
    {
      const std::scoped_lock lock(heard().lock);
      heard().bytes.clear();
      heard().gaps = 0;
    }
    Recording recording;
    MediaPlayer player(AudioOutput::System);
    std::vector<float> samples(kTapLength);
    TEST_CHECK(player.open(clip.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(!player.soundDeviceMissing());
    player.play();
    int round = 0;
    const double start = processorMs();
    TEST_CHECK(pump.until([&] {
      during(player, round++);
      recording.windows += player.copyAudioWindow(samples).count > 0 ? 1 : 0;
      return player.state() == MediaState::Ended;
    }));
    recording.busyMs = processorMs() - start;
    const std::scoped_lock lock(heard().lock);
    recording.size = heard().bytes.size();
    recording.gaps = heard().gaps;
    recording.hash = 14695981039346656037ULL;
    for (const std::uint8_t byte : heard().bytes) {
      recording.hash = (recording.hash ^ byte) * 1099511628211ULL;
    }
    return recording;
  }

  // The device plays the same sound, without gaps, with the tap off, on, and switched on and off while playing.
  void leavesTheSoundAlone(Pump& pump, const fs::path& clip) {
    const auto off = playThrough(pump, clip, [](MediaPlayer& /*player*/, int /*round*/) {});
    const auto on = playThrough(pump, clip, [](MediaPlayer& player, int round) {
      if (round == 0) {
        TEST_CHECK(player.startAudioTap(kTapRate, kTapLength));
      }
    });
    // Switched every round at first, then off for 100 ms of each 2.5 s: a tap turned on gets its first samples once
    // the sound that passed it after that is heard, the player's one-second audio queue and this output's 300 ms
    // later.
    const auto started = Clock::now();
    const auto switched = playThrough(pump, clip, [started](MediaPlayer& player, int round) {
      const auto phase = std::chrono::duration_cast<milliseconds>(Clock::now() - started).count() % 2500;
      if (round < 100 ? round % 2 == 0 : phase >= 100) {
        TEST_CHECK(player.startAudioTap(kTapRate, kTapLength));
      } else {
        player.stopAudioTap();
      }
    });
    std::println(
        "played {} bytes with the tap off, {} on, {} switched; {} and {} copies held samples; processor {:.0f} ms off, "
        "{:.0f} ms on",
        off.size, on.size, switched.size, on.windows, switched.windows, off.busyMs, on.busyMs
    );
    TEST_CHECK(::test::withinBudget(on.busyMs - off.busyMs, 100));
    TEST_CHECK(off.size > 0 && off.gaps == 0 && off.windows == 0);
    TEST_CHECK(on.size == off.size && on.hash == off.hash && on.gaps == 0 && on.windows > 0);
    TEST_CHECK(switched.size == off.size && switched.hash == off.hash && switched.gaps == 0 && switched.windows > 0);
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
  const auto toneClip = folder / "stereo-tone.wav";
  writeWav(toneClip, 44100, 2, 3, [](int channel, double t) {
    return 0.5 * std::sin(2 * std::numbers::pi * (channel == 0 ? 440 : 660) * t);
  });
  useLateSoundOutput();
  leavesTheSoundAlone(pump, toneClip);
  std::println("media-audio-tap-output: all checks passed");
  return 0;
}
