#include "media/media_player.h"
#include "media/pipeline_thread.h"
#include "media_clips.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The tap on the player's sound: mono samples at the asked rate, in step with what is heard, through a sink that
// plays nothing.
namespace {
  using luciole::AudioOutput;
  using luciole::AudioWindow;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using namespace test::media;

  using Clock = std::chrono::steady_clock;
  using std::chrono::milliseconds;

  constexpr int kRate = kTapRate;
  constexpr int kLength = kTapLength;
  // How far the window may end from where the sound is heard, in seconds.
  constexpr double kInStep = 0.030;

  // The ramp clip rises steadily, so each sample tells the second it was taken from.
  double ramp(double seconds) { return -0.8 + 0.4 * seconds; }
  double rampTime(float value) { return (value + 0.8) / 0.4; }

  // The frequency of the rising zero crossings in `samples` at kRate; 0 with fewer than two.
  double crossingFrequency(std::span<const float> samples) {
    double first = -1;
    double last = -1;
    int rises = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
      if (samples[i - 1] < 0 && samples[i] >= 0) {
        const double at = static_cast<double>(i - 1) + samples[i - 1] / (samples[i - 1] - samples[i]);
        first = rises == 0 ? at : first;
        last = at;
        ++rises;
      }
    }
    return rises < 2 ? 0 : (rises - 1) * kRate / (last - first);
  }

  float peak(std::span<const float> samples) {
    float most = 0;
    for (const float value : samples) {
      most = std::max(most, std::abs(value));
    }
    return most;
  }

  // The probes GStreamer reports adding to and removing from the player's "sound-tap" element, whose output pad the
  // tap watches.
  struct ProbeCount {
    std::atomic<int> added{0};
    std::atomic<int> removed{0};
  };
  ProbeCount& probes() {
    static ProbeCount instance;
    return instance;
  }

  // Called on any thread. Both functions log with the pad's object lock held, so the parent is read without it.
  void countProbes(
      GstDebugCategory* /*category*/, GstDebugLevel /*level*/, const gchar* /*file*/, const gchar* function,
      gint /*line*/, GObject* object, GstDebugMessage* message, gpointer /*data*/
  ) {
    const std::string_view name = function != nullptr ? function : "";
    const bool adding = name == "gst_pad_add_probe";
    if ((!adding && name != "gst_pad_remove_probe") || object == nullptr || !GST_IS_PAD(object)) {
      return;
    }
    GstObject* parent = GST_OBJECT_PARENT(object);
    if (parent == nullptr || std::string_view(GST_OBJECT_NAME(parent)) != "sound-tap") {
      return;
    }
    const std::string_view text = gst_debug_message_get(message);
    if (adding && text.starts_with("got probe id")) {
      ++probes().added;
    } else if (!adding && text.starts_with("removing hook")) {
      ++probes().removed;
    }
  }

  // Counts probes on the tap's pad from GStreamer's log while it lives, printing nothing. The probe
  // functions log in these two categories.
  class ProbeWatch {
  public:
    ProbeWatch() {
      probes().added = 0;
      probes().removed = 0;
      gst_debug_add_log_function(countProbes, nullptr, nullptr);
      (void)(gst_debug_remove_log_function(gst_debug_log_default));
      for (const char* category : kCategories) {
        gst_debug_set_threshold_for_name(category, GST_LEVEL_LOG);
      }
    }
    ~ProbeWatch() {
      for (const char* category : kCategories) {
        gst_debug_unset_threshold_for_name(category);
      }
      gst_debug_add_log_function(gst_debug_log_default, nullptr, nullptr);
      (void)(gst_debug_remove_log_function(countProbes));
    }
    ProbeWatch(const ProbeWatch&) = delete;
    ProbeWatch& operator=(const ProbeWatch&) = delete;

  private:
    static constexpr std::array kCategories{"GST_PADS", "GST_SCHEDULING"};
  };

  void refusesWhatItCannotTap() {
    MediaPlayer player(AudioOutput::Silent);
    for (const auto& [rate, length] : std::array<std::pair<int, int>, 8>{
             {{0, kLength},
              {-kRate, kLength},
              {999, 100},
              {192001, kLength},
              {kRate, 0},
              {kRate, -1},
              {kRate, kRate + 1},
              {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}}
         }) {
      TEST_CHECK(!player.startAudioTap(rate, length));
    }
    std::vector<float> samples(kLength, 7.0F);
    const auto window = player.copyAudioWindow(samples);
    TEST_CHECK(window.count == 0 && !window.fresh);
    TEST_CHECK(samples.front() == 7.0F);
    TEST_CHECK(player.startAudioTap(1000, 1000));
    TEST_CHECK(player.startAudioTap(192000, kLength));
    // Nothing to copy into, and nothing open.
    TEST_CHECK(player.copyAudioWindow({}).count == 0);
    TEST_CHECK(player.copyAudioWindow(samples).count == 0);
    player.stopAudioTap();
  }

  // A steady tone arrives at the asked rate whatever the file's own rate, and a stereo file is mixed to mono.
  void deliversTheToneAtTheAskedRate(Pump& pump, const fs::path& folder) {
    for (const int rate : {8000, 44100, 48000, 96000}) {
      const auto path = folder / std::format("tone-{}.wav", rate);
      writeWav(path, rate, 1, 1.5, [](int /*channel*/, double t) {
        return 0.5 * std::sin(2 * std::numbers::pi * 1000 * t);
      });
      Tapped tapped;
      TEST_CHECK(tapped.player.open(path.string()));
      tapped.player.play();
      TEST_CHECK(tapped.full(pump));
      const double frequency = crossingFrequency(tapped.valid());
      std::println("{} Hz file: {:.1f} Hz, peak {:.3f}", rate, frequency, peak(tapped.valid()));
      TEST_NEAR(frequency, 1000.0, 5.0);
      TEST_NEAR(peak(tapped.valid()), 0.5F, 0.03F);
    }
    // Left louder than right, in phase: the mono mix is their mean.
    const auto stereo = folder / "stereo.wav";
    writeWav(stereo, 44100, 2, 1.5, [](int channel, double t) {
      return (channel == 0 ? 0.6 : 0.2) * std::sin(2 * std::numbers::pi * 1000 * t);
    });
    Tapped tapped;
    TEST_CHECK(tapped.player.open(stereo.string()));
    tapped.player.play();
    TEST_CHECK(tapped.full(pump));
    std::println("stereo file: {:.1f} Hz, peak {:.3f}", crossingFrequency(tapped.valid()), peak(tapped.valid()));
    TEST_NEAR(crossingFrequency(tapped.valid()), 1000.0, 5.0);
    TEST_NEAR(peak(tapped.valid()), 0.4F, 0.03F);
  }

  // The time of each valid sample of the ramp, checked to be one unbroken run.
  struct Span {
    double first = 0;
    double last = 0;
  };
  Span rampSpan(const Tapped& tapped) {
    const auto valid = tapped.valid();
    const Span span{.first = rampTime(valid.front()), .last = rampTime(valid.back())};
    TEST_NEAR(span.last - span.first, static_cast<double>(valid.size() - 1) / kRate, 0.005);
    return span;
  }

  // Copies a window between two readings of the position: how far its end lies outside them, in seconds.
  double outOfStep(Tapped& tapped) {
    const double before = tapped.player.position();
    (void)tapped.copy();
    const double after = tapped.player.position();
    if (tapped.window.count == 0) {
      return 0;
    }
    const double end = rampSpan(tapped).last;
    return std::max({0.0, before - end, end - after});
  }

  // The window ends where the sound is heard, also when the output takes the sound well before it is heard, and it
  // follows the clock: two seconds of playing deliver two seconds of samples.
  void staysInStep(Pump& pump, const fs::path& clip, AudioOutput output) {
    Tapped tapped(output);
    auto& player = tapped.player;
    TEST_CHECK(player.open(clip.string()));
    player.play();
    TEST_CHECK(pump.until([&] { return tapped.copy().count == kLength && player.position() >= 0.5; }));
    double worst = 0;
    struct Mark {
      double heard = 0;
      Clock::time_point from;
      Clock::time_point to;
    };
    std::optional<Mark> first;
    Mark last;
    // Each round pumps at least a poll; the count bounds the loop and the clock ends it after two seconds.
    for (int round = 0; round < 4000 && (!first || Clock::now() - first->from < milliseconds(2000)); ++round) {
      pump.run(milliseconds(16));
      const auto from = Clock::now();
      worst = std::max(worst, outOfStep(tapped));
      const auto to = Clock::now();
      if (tapped.window.count == kLength) {
        last = {.heard = rampSpan(tapped).last, .from = from, .to = to};
        first = first.value_or(last);
      }
    }
    TEST_CHECK(first.has_value());
    const auto seconds = [](Clock::duration span) { return std::chrono::duration<double>(span).count(); };
    const double delivered = (last.heard - first->heard) * kRate;
    const double shortest = seconds(last.from - first->to) * kRate;
    const double longest = seconds(last.to - first->from) * kRate;
    std::println(
        "{}: window end at most {:.1f} ms from the heard position; {:.0f} samples delivered in {:.0f} to {:.0f}",
        output == AudioOutput::Silent ? "silent output" : "output that takes sound early", worst * 1000, delivered,
        shortest, longest
    );
    TEST_CHECK(worst < kInStep);
    TEST_CHECK(delivered > shortest * 0.98 && delivered < longest * 1.02);
    player.close();
    drainPipelineThread(pump);
    TEST_CHECK(!holdsOpen(clip));
  }

  // After a seek, every window holds only sound from the new place on, in step.
  void startsAgainAt(Pump& pump, Tapped& tapped, double target) {
    TEST_CHECK(pump.until([&] { return tapped.copy().fresh && rampSpan(tapped).last < target + 0.2; }));
    int full = 0;
    // Each round pumps at least a poll; the count bounds the loop and the clock ends it.
    const auto until = Clock::now() + milliseconds(500);
    for (int round = 0; round < 1000 && Clock::now() < until; ++round) {
      pump.once();
      TEST_CHECK(outOfStep(tapped) < kInStep);
      if (tapped.window.count > 0) {
        TEST_CHECK(rampSpan(tapped).first >= target - 0.01);
        full += tapped.window.count == kLength ? 1 : 0;
      }
    }
    TEST_CHECK(full > 0);
  }

  // Paused, the window holds still and says so; playing again moves it on; a seek drops what came before it.
  void holdsWhilePausedAndStartsAgainAfterASeek(Pump& pump, const fs::path& clip) {
    Tapped tapped;
    auto& player = tapped.player;
    TEST_CHECK(player.open(clip.string()));
    TEST_CHECK(reached(pump, player, MediaState::Paused));
    TEST_CHECK(tapped.copy().count == 0); // paused at the start, nothing was heard yet
    player.play();
    TEST_CHECK(pump.until([&] { return tapped.copy().count == kLength && player.position() >= 1.0; }));
    player.pause();
    pump.run(milliseconds(200));
    TEST_CHECK(tapped.copy().count == kLength);
    const float held = tapped.samples.back();
    pump.run(milliseconds(300));
    TEST_CHECK(tapped.copy().count == kLength && !tapped.window.fresh);
    TEST_CHECK(tapped.samples.back() == held);
    TEST_CHECK(!tapped.copy().fresh);
    player.play();
    TEST_CHECK(pump.until([&] { return tapped.copy().fresh && tapped.samples.back() > held; }));

    player.seek(0.5);
    startsAgainAt(pump, tapped, 0.5);
    player.pause();
    pump.run(milliseconds(200));
    player.seek(2.5);
    pump.run(milliseconds(300));
    if (tapped.copy().count > 0) {
      TEST_CHECK(rampSpan(tapped).first >= 2.49);
    }
    player.play();
    startsAgainAt(pump, tapped, 2.5);
  }

  // Muted or turned down, the window still holds the file's own sound at its full level.
  void hearsTheFileWhileMutedOrQuiet(Pump& pump, const fs::path& clip) {
    Tapped tapped;
    auto& player = tapped.player;
    player.setMuted(true);
    TEST_CHECK(player.open(clip.string()));
    player.play();
    TEST_CHECK(pump.until([&] { return tapped.copy().count == kLength && player.position() >= 1.0; }));
    std::println(
        "muted: newest sample {:.3f} at {:.3f} s, peak {:.3f}", tapped.samples.back(), player.position(),
        peak(tapped.valid())
    );
    TEST_CHECK(peak(tapped.valid()) > 0.3F);
    TEST_CHECK(std::abs(rampTime(tapped.samples.back()) - player.position()) < 0.1);
    player.setMuted(false);
    player.setVolume(0.05);
    const double turned = player.position();
    TEST_CHECK(pump.until([&] { return tapped.copy().count == kLength && player.position() >= turned + 0.5; }));
    // About -0.2 here; turned down, it would be a twentieth of that.
    TEST_CHECK(peak(tapped.valid()) > 0.15F);
    TEST_CHECK(std::abs(rampTime(tapped.samples.back()) - player.position()) < 0.1);
  }

  // Off, nothing is copied; turned on while playing, the window holds only sound heard since.
  void gathersNothingWhileOff(Pump& pump, const fs::path& clip) {
    MediaPlayer player(AudioOutput::Silent);
    std::vector<float> samples(kLength, 7.0F);
    TEST_CHECK(player.open(clip.string()));
    player.play();
    TEST_CHECK(pump.until([&] { return player.position() >= 1.0; }));
    for (int i = 0; i < 20; ++i) {
      pump.once();
      const auto window = player.copyAudioWindow(samples);
      TEST_CHECK(window.count == 0 && !window.fresh);
    }
    TEST_CHECK(samples.front() == 7.0F && samples.back() == 7.0F);
    const double on = player.position();
    TEST_CHECK(player.startAudioTap(kRate, kLength));
    TEST_CHECK(pump.until([&] { return player.copyAudioWindow(samples).count > 0; }));
    TEST_CHECK(rampTime(samples.front()) >= on - 0.01);
    player.stopAudioTap();
    samples.assign(kLength, 7.0F);
    pump.run(milliseconds(100));
    TEST_CHECK(player.copyAudioWindow(samples).count == 0);
    TEST_CHECK(samples.front() == 7.0F);
  }

  // Stopped while playing, the tap takes its probe off the sound pad, so no sound passes through it while it is off;
  // turned on again, it puts one back.
  void takesItsProbeOffWhenStopped(Pump& pump, const fs::path& clip) {
    drainPipelineThread(pump);
    const ProbeWatch watch;
    Tapped tapped(AudioOutput::System);
    TEST_CHECK(tapped.player.open(clip.string()));
    tapped.player.play();
    TEST_CHECK(tapped.full(pump));
    TEST_CHECK(probes().added == 1 && probes().removed == 0);
    tapped.player.stopAudioTap();
    TEST_CHECK(pump.until([] { return probes().removed == 1; }));
    TEST_CHECK(tapped.player.startAudioTap(kRate, kLength));
    TEST_CHECK(tapped.full(pump));
    TEST_CHECK(probes().added == 2 && probes().removed == 1);
    tapped.player.stopAudioTap();
    TEST_CHECK(pump.until([] { return probes().removed == 2; }));
    pump.run(milliseconds(100));
    std::println("probes on the sound pad: {} added, {} removed", probes().added.load(), probes().removed.load());
    tapped.player.close();
    drainPipelineThread(pump);
    TEST_CHECK(probes().added == 2 && probes().removed == 2);
  }

  // Closing frees the samples in any state, the tap stays on for the next file, and a player destroyed while it plays
  // with the tap on lets go of its file.
  void releasesEverythingOnClose(Pump& pump, const fs::path& clip) {
    {
      Tapped tapped;
      TEST_CHECK(tapped.player.open(clip.string()));
      tapped.player.play();
      TEST_CHECK(tapped.full(pump));
      tapped.player.close();
      TEST_CHECK(tapped.copy().count == 0);
      TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
      TEST_CHECK(tapped.player.open(clip.string()));
      tapped.player.close(); // while it loads
      TEST_CHECK(tapped.player.open(clip.string()));
      tapped.player.play();
      TEST_CHECK(tapped.full(pump));
    }
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }

  // With no sound device a sound file cannot be played, so the tap has nothing.
  void hasNothingWithoutASoundDevice(Pump& pump, const fs::path& clip) {
    breakSystemSoundOutput();
    Tapped tapped(AudioOutput::System);
    TEST_CHECK(tapped.player.open(clip.string()));
    TEST_CHECK(pump.until([&] { return tapped.player.state() == MediaState::Error; }));
    TEST_CHECK(tapped.player.soundDeviceMissing());
    TEST_CHECK(tapped.copy().count == 0);
  }
} // namespace

int main() {
  if (!MediaPlayer::available()) {
    MediaPlayer player;
    TEST_CHECK(!player.startAudioTap(kRate, kLength));
    std::vector<float> samples(kLength);
    TEST_CHECK(player.copyAudioWindow(samples).count == 0);
    std::println("media-audio-tap: playback not built");
    return 0;
  }
  Scratch scratch;
  if (const auto missing = missingElements({"wavparse", "playbin", "appsink", "fakesink", "queue"}); !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  Pump pump;
  const auto folder = scratch.path("clips");
  fs::create_directory(folder);
  const auto rampClip = folder / "ramp.wav";
  writeWav(rampClip, 48000, 1, 4, [](int /*channel*/, double t) { return ramp(t); });
  refusesWhatItCannotTap();
  deliversTheToneAtTheAskedRate(pump, folder);
  staysInStep(pump, rampClip, AudioOutput::Silent);
  holdsWhilePausedAndStartsAgainAfterASeek(pump, rampClip);
  hearsTheFileWhileMutedOrQuiet(pump, rampClip);
  gathersNothingWhileOff(pump, rampClip);
  releasesEverythingOnClose(pump, rampClip);
  useLateSoundOutput();
  staysInStep(pump, rampClip, AudioOutput::System);
  takesItsProbeOffWhenStopped(pump, rampClip);
  hasNothingWithoutASoundDevice(pump, rampClip);
  std::println("media-audio-tap: all checks passed");
  return 0;
}
