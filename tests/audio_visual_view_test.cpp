#include "audio_visual_clips.h"
#include "cpu_budget.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "png_writer.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/audio_visual_view.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <print>
#include <string>
#include <vector>

// The visual of a sound preview on a real player that plays generated sound through the silent output, drawn
// offscreen one frame at a time the way a surface draws: lay out and draw, then clear the dirty flags.
namespace {
  using luciole::AudioOutput;
  using luciole::AudioVisualKind;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using std::chrono::milliseconds;
  using namespace test::media;

  constexpr int kWidth = 320;
  constexpr int kHeight = 200;

  class Holder final : public Node {
    void doLayout(Renderer& renderer) override {
      for (const auto& child : children()) {
        child->arrange(renderer, {0, 0, width(), height()});
      }
    }
  };

  double threadMs() {
    timespec now{};
    TEST_CHECK(::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1e3) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  void keep(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir != nullptr && dir[0] != '\0') {
      TEST_CHECK(::test::writePng(pixels, std::filesystem::path(dir) / name));
    }
  }

  int differing(const ::test::GlPixels& a, const ::test::GlPixels& b) {
    TEST_CHECK(a.rgba.size() == b.rgba.size());
    int count = 0;
    for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4) {
      count += a.rgba[i] != b.rgba[i] || a.rgba[i + 1] != b.rgba[i + 1] || a.rgba[i + 2] != b.rgba[i + 2] ? 1 : 0;
    }
    return count;
  }

  // Pixels brighter than `luma` (0 to 255).
  int bright(const ::test::GlPixels& pixels, int luma) {
    int count = 0;
    for (std::size_t i = 0; i + 3 < pixels.rgba.size(); i += 4) {
      const int value = ((pixels.rgba[i] * 299) + (pixels.rgba[i + 1] * 587) + (pixels.rgba[i + 2] * 114)) / 1000;
      count += value > luma ? 1 : 0;
    }
    return count;
  }

  struct Host {
    ::test::SceneCapture& capture;
    Holder root;
    AudioVisualView* view = nullptr;
    MediaPlayer player{AudioOutput::Silent};
    Pump pump;
    std::vector<float> samples = std::vector<float>(AudioVisualView::kTapLength);
    double lastDrawMs = 0;

    explicit Host(::test::SceneCapture& rig) : capture(rig) {
      view = static_cast<AudioVisualView*>(root.addChild(std::make_unique<AudioVisualView>()));
      player.setChangedCallback([this] { view->follow(&player, true); });
    }
    ~Host() { player.setChangedCallback({}); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // One displayed frame: the main context and the timers run, then the scene draws and its flags are cleared.
    ::test::GlPixels frame() {
      pump.once();
      const double start = threadMs();
      auto pixels = capture.capture(root, kWidth, kHeight, 1);
      lastDrawMs = threadMs() - start;
      root.clearDirty();
      return pixels;
    }
    // Frames every 16 ms for `span`, as a 60 Hz display draws them while playing; the last one.
    ::test::GlPixels framesFor(milliseconds span) {
      const auto start = std::chrono::steady_clock::now();
      auto last = frame();
      // Each frame waits 16 ms first, so the count bounds the loop beside the clock.
      for (std::int64_t i = 0; i <= span.count() / 16 && std::chrono::steady_clock::now() - start < span; ++i) {
        pump.run(milliseconds(16));
        last = frame();
      }
      return last;
    }
    // Whether the scene asks to be drawn again within `span`.
    bool asksForAFrame(milliseconds span = milliseconds(100)) {
      return pump.until([this] { return root.paintDirty(); }, span);
    }
    bool until(const std::function<bool()>& done) { return pump.until(done); }
    // Play and pause change the state at once without the changed callback; an owner follows after them.
    void play() {
      player.play();
      view->follow(&player, true);
    }
    void pause() {
      player.pause();
      view->follow(&player, true);
    }
    void play(const fs::path& clip) {
      TEST_CHECK(player.open(clip.string()));
      view->follow(&player, true);
      play();
      TEST_CHECK(until([this] { return player.state() == MediaState::Playing && view->active(); }));
    }
    // Copies a window from the tap: none while it is off.
    int tapped() { return player.copyAudioWindow(samples).count; }
  };

  // Before Play nothing runs and nothing draws; playing, each frame changes the picture and asks for the next.
  void movesWhilePlaying(Host& host, const fs::path& clip) {
    TEST_CHECK(host.player.open(clip.string()));
    host.view->follow(&host.player, true);
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Paused; }));
    TEST_CHECK(!host.view->active() && !host.view->running());
    const auto idle = host.frame();
    TEST_CHECK(bright(idle, 4) == 0); // the view draws nothing at all
    TEST_CHECK(!host.asksForAFrame());
    host.play();
    TEST_CHECK(host.until([&] { return host.view->active() && host.view->running(); }));
    TEST_CHECK(host.until([&] { return host.tapped() == AudioVisualView::kTapLength; }));
    const auto before = host.frame();
    TEST_CHECK(host.asksForAFrame());
    const auto after = host.framesFor(milliseconds(300));
    TEST_CHECK(host.asksForAFrame());
    std::println(
        "playing: {} of {} pixels changed in 300 ms, {} drawn", differing(before, after), kWidth * kHeight,
        bright(after, 4)
    );
    keep(before, "view-playing-first.png");
    keep(after, "view-playing.png");
    TEST_CHECK(differing(before, after) > 100);          // paused, not one pixel changes
    TEST_CHECK(bright(after, 4) > kWidth * kHeight / 2); // the background covers the box
  }

  // Paused, the same picture draws again and no frame is asked for; playing again moves it on.
  void holdsWhilePaused(Host& host) {
    host.pause();
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Paused; }));
    TEST_CHECK(host.view->active() && host.view->running());
    const auto first = host.frame();
    TEST_CHECK(!host.asksForAFrame(milliseconds(300)));
    const auto second = host.frame();
    TEST_CHECK(differing(first, second) == 0);
    TEST_CHECK(!host.asksForAFrame(milliseconds(100)));
    keep(second, "view-paused.png");
    host.play();
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Playing; }));
    TEST_CHECK(differing(second, host.framesFor(milliseconds(300))) > 100);
  }

  // Pixels that differ from the one at the corner, the background.
  int markedPixels(const ::test::GlPixels& pixels) {
    int count = 0;
    for (std::size_t i = 4; i + 3 < pixels.rgba.size(); i += 4) {
      count += pixels.rgba[i] != pixels.rgba[0]
              || pixels.rgba[i + 1] != pixels.rgba[1]
              || pixels.rgba[i + 2] != pixels.rgba[2]
          ? 1
          : 0;
    }
    return count;
  }

  // A visual chosen while paused advances once on the window the tap holds, so the stage is not empty until Play.
  void swapsWhilePaused(Host& host) {
    host.pause();
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Paused; }));
    host.view->setChoice({.on = true, .kind = AudioVisualKind::Bars});
    const auto first = host.frame();
    TEST_CHECK(markedPixels(first) > 20000); // an unadvanced visual marks about 4000
    TEST_CHECK(differing(first, host.frame()) == 0);
    TEST_CHECK(!host.asksForAFrame(milliseconds(100)));
    host.play();
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Playing; }));
  }

  // Off, the tap is stopped, the visual is gone and nothing draws or asks; on again, it runs at once.
  void turnsOffAndOn(Host& host) {
    host.view->setChoice({.on = false, .kind = AudioVisualKind::CosmicPeanut});
    TEST_CHECK(!host.view->active() && !host.view->running());
    TEST_CHECK(host.tapped() == 0);
    TEST_CHECK(bright(host.frame(), 4) == 0);
    TEST_CHECK(!host.asksForAFrame());
    host.pump.run(milliseconds(200));
    TEST_CHECK(host.tapped() == 0);
    host.view->setChoice({.on = true, .kind = AudioVisualKind::CosmicPeanut});
    TEST_CHECK(host.view->active() && host.view->running());
    TEST_CHECK(host.root.paintDirty());
    (void)host.frame();
    TEST_CHECK(host.asksForAFrame());
    TEST_CHECK(host.until([&] { return host.tapped() > 0; }));
  }

  // Each kind takes over at once while playing and moves. Frames come every 16 ms, as on a 60 Hz display; the
  // processor time of drawing them is measured against the same frames with the visualizer off.
  void switchesEveryKind(Host& host) {
    constexpr int kFrames = 90;
    const auto paced = [&host](int frames, double& totalMs, double& worstMs) {
      ::test::GlPixels last;
      for (int i = 0; i < frames; ++i) {
        host.pump.run(milliseconds(16));
        last = host.frame();
        totalMs += host.lastDrawMs;
        worstMs = std::max(worstMs, host.lastDrawMs);
      }
      return last;
    };
    host.view->setChoice({.on = false, .kind = AudioVisualKind::CosmicPeanut});
    double offMs = 0;
    double offWorstMs = 0;
    (void)paced(kFrames / 3, offMs, offWorstMs);
    offMs /= kFrames / 3;
    for (const auto kind : luciole::kAudioVisualKinds) {
      host.view->setChoice({.on = true, .kind = kind});
      TEST_CHECK(host.view->running() && host.view->choice().kind == kind);
      double totalMs = 0;
      double worstMs = 0;
      const auto middle = paced(kFrames / 2, totalMs, worstMs);
      const auto last = paced(kFrames / 2, totalMs, worstMs);
      const double meanMs = totalMs / kFrames;
      std::println(
          "{}: {:.2f} ms of processor per frame (worst {:.2f}), {:.2f} ms with the visualizer off; {} pixels moved "
          "in 0.75 s",
          luciole::audioVisualName(kind), meanMs, worstMs, offMs, differing(middle, last)
      );
      TEST_CHECK(differing(middle, last) > 100);
      TEST_CHECK(::test::withinBudget(meanMs, 25));
      keep(last, std::format("view-{}.png", luciole::audioVisualName(kind)));
    }
  }

  // After the context is lost the visual forgets its GL objects and makes them again on the next draw.
  void survivesALostContext(Host& host) {
    host.view->setChoice({.on = true, .kind = AudioVisualKind::Bars});
    const auto before = host.frame();
    test::audio::GpuResetRenderer renderer(host.capture.textureManager());
    host.root.invalidateGpuResources(renderer, 7);
    TEST_CHECK(host.view->running());
    const auto after = host.framesFor(milliseconds(200));
    TEST_CHECK(bright(after, 4) > kWidth * kHeight / 2);
    TEST_CHECK(differing(before, after) > 100);
    TEST_CHECK(host.asksForAFrame());
  }

  // Muted, the visual still hears the file: the bars stand far taller than for a silent file.
  void hearsAMutedFile(Host& host, const fs::path& clip, const fs::path& silence) {
    host.view->setChoice({.on = true, .kind = AudioVisualKind::Bars});
    const auto barsAfterASecond = [&](const fs::path& file, bool muted) {
      host.player.setMuted(muted);
      host.play(file);
      const auto start = std::chrono::steady_clock::now();
      ::test::GlPixels last;
      while (std::chrono::steady_clock::now() - start < milliseconds(1200)) {
        last = host.frame();
      }
      return bright(last, 110);
    };
    const int quiet = barsAfterASecond(silence, false);
    const int muted = barsAfterASecond(clip, true);
    std::println("bright pixels after a second: {} for silence, {} for the muted tone", quiet, muted);
    TEST_CHECK(muted > (quiet * 3) + 300);
    host.player.setMuted(false);
  }

  // At the end and without a player the view stops: no visual, no tap, no frame.
  void stopsAtTheEnd(Host& host) {
    host.player.seek(host.player.duration() - 0.2);
    TEST_CHECK(host.until([&] { return host.player.state() == MediaState::Ended; }));
    TEST_CHECK(!host.view->active() && !host.view->running());
    TEST_CHECK(host.tapped() == 0);
    TEST_CHECK(bright(host.frame(), 4) == 0);
    TEST_CHECK(!host.asksForAFrame());
    host.play(); // from the end it starts again
    TEST_CHECK(host.until([&] { return host.view->running(); }));
    host.player.setChangedCallback({}); // the owner lets go of its player
    host.view->follow(nullptr, true);
    TEST_CHECK(!host.view->active() && !host.view->running());
    TEST_CHECK(bright(host.frame(), 4) == 0);
    TEST_CHECK(!host.asksForAFrame());
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements({"wavparse", "playbin", "fakesink", "identity"}); !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  test::audio::useDarkTheme();
  const auto clip = scratch.path("tone-and-kicks.wav");
  const auto silence = scratch.path("silence.wav");
  test::audio::writeSoundClip(clip, test::audio::SoundClip::ToneAndKicks, 40);
  test::audio::writeSoundClip(silence, test::audio::SoundClip::Silence, 3);
  {
    Host host(*capture);
    movesWhilePlaying(host, clip);
    holdsWhilePaused(host);
    swapsWhilePaused(host);
    turnsOffAndOn(host);
    switchesEveryKind(host);
    survivesALostContext(host);
    hearsAMutedFile(host, clip, silence);
    stopsAtTheEnd(host);
  }
  std::println("audio-visual-view: all checks passed");
  return 0;
}
