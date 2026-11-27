#include "audio_visual_clips.h"
#include "core/input/key_modifiers.h"
#include "media_clips.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <variant>
#include <vector>

// The sound preview with its visual: the stage it takes, the two buttons in the control bar and what they report to
// the owner, driven through the input dispatcher over a real media view that plays generated sound silently.
namespace {
  using luciole::AudioOutput;
  using luciole::AudioVisualKind;
  using luciole::MediaState;
  using std::chrono::milliseconds;
  using namespace test::media;

  constexpr int kWidth = 360;
  constexpr int kHeight = 260;

  class Holder final : public Node {
    void doLayout(Renderer& renderer) override {
      for (const auto& child : children()) {
        child->arrange(renderer, {0, 0, width(), height()});
      }
    }
  };

  std::string tooltipOf(const Button& button) {
    const auto content = button.inputArea()->tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  // Pixels of the stage, the view's area above the control bar, that differ between two frames.
  int stageDiffering(const ::test::GlPixels& a, const ::test::GlPixels& b, int stageRows) {
    int count = 0;
    for (int y = 0; y < stageRows; ++y) {
      for (int x = 0; x < a.width; ++x) {
        const auto p = a.at(x, y);
        const auto q = b.at(x, y);
        count += p[0] != q[0] || p[1] != q[1] || p[2] != q[2] ? 1 : 0;
      }
    }
    return count;
  }

  struct Rig {
    ::test::SceneCapture& capture;
    AnimationManager animations;
    Holder root;
    MediaView* view = nullptr;
    InputDispatcher dispatcher;
    Pump pump;
    std::uint32_t serial = 0;
    std::uint32_t modifiers = 0;
    std::vector<AudioVisualChoice> chosen;

    explicit Rig(::test::SceneCapture& rig) : capture(rig) {
      root.setAnimationManager(&animations);
      view = static_cast<MediaView*>(
          root.addChild(std::make_unique<MediaView>(ColorRole::SurfaceContainerLow, AudioOutput::Silent))
      );
      view->audioVisual().onChosen = [this](const AudioVisualChoice& choice) { chosen.push_back(choice); };
      view->audioVisual().setModifiers([this] { return modifiers; });
      dispatcher.setSceneRoot(&root);
      (void)frame();
    }

    // One displayed frame, its dirty flags cleared as a surface clears them.
    ::test::GlPixels frame() {
      pump.once();
      animations.tick(16);
      auto pixels = capture.capture(root, kWidth, kHeight, 1);
      root.clearDirty();
      return pixels;
    }
    bool until(const std::function<bool()>& done, milliseconds limit = kWait) {
      const auto deadline = std::chrono::steady_clock::now() + limit;
      for (std::int64_t i = 0; i < limit.count() * 100 && std::chrono::steady_clock::now() < deadline; ++i) {
        if (done()) {
          return true;
        }
        (void)frame();
      }
      return done();
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
    bool asksForAFrame(milliseconds span) {
      return pump.until([this] { return root.paintDirty(); }, span);
    }
    int stageRows() const { return static_cast<int>(view->audioVisual().height()); }
    MediaState state() const { return view->player() != nullptr ? view->player()->state() : MediaState::Idle; }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      TEST_CHECK(node.visible() && node.width() > 0 && node.height() > 0);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
      (void)frame();
    }
  };

  // Before Play a sound file shows its glyph and the two buttons; playing, the visual takes the stage and moves.
  void takesTheStageWhilePlaying(Rig& rig, const fs::path& clip) {
    auto& visual = rig.view->audioVisual();
    rig.view->setSource(clip.string());
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }));
    TEST_CHECK(visual.visualizerButton().visible() && visual.visualButton().visible());
    TEST_CHECK(tooltipOf(visual.visualizerButton()) == "Turn off the audio visualizer");
    TEST_CHECK(
        tooltipOf(visual.visualButton()) == "Visual: Cosmic Peanut (click for the next, Shift-click for the previous)"
    );
    TEST_CHECK(!visual.active() && !visual.running());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && visual.active(); }));
    TEST_CHECK(!rig.view->showsSoundGlyph());
    const auto first = rig.frame();
    const auto later = rig.framesFor(milliseconds(600));
    const int moved = stageDiffering(first, later, rig.stageRows());
    std::println("playing: {} stage pixels moved in 0.6 s", moved);
    TEST_CHECK(moved > 100);
    TEST_CHECK(rig.asksForAFrame(milliseconds(100)));
  }

  // Paused after playing, the picture stays and asks for nothing; the glyph does not come back.
  void holdsWhilePaused(Rig& rig) {
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(rig.view->audioVisual().active() && !rig.view->showsSoundGlyph());
    const auto first = rig.frame();
    rig.pump.run(milliseconds(300));
    const auto second = rig.frame();
    TEST_CHECK(stageDiffering(first, second, rig.stageRows()) == 0);
    TEST_CHECK(!rig.asksForAFrame(milliseconds(300)));
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.state() == MediaState::Playing);
  }

  // The visual button steps forward, and back with Shift, the visualizer button turns it off and on; each click
  // reports the new choice to the owner at once.
  void buttonsChangeTheChoice(Rig& rig) {
    auto& visual = rig.view->audioVisual();
    rig.click(visual.visualButton());
    TEST_CHECK(visual.choice().kind == AudioVisualKind::Pulsar && visual.running());
    TEST_CHECK(rig.chosen.size() == 1 && rig.chosen.back().kind == AudioVisualKind::Pulsar && rig.chosen.back().on);
    TEST_CHECK(tooltipOf(visual.visualButton()) == "Visual: Pulsar (click for the next, Shift-click for the previous)");
    rig.modifiers = KeyMod::Shift;
    rig.click(visual.visualButton());
    rig.click(visual.visualButton());
    rig.modifiers = 0;
    TEST_CHECK(visual.choice().kind == AudioVisualKind::Stargate);
    TEST_CHECK(rig.chosen.size() == 3 && rig.chosen.back().kind == AudioVisualKind::Stargate);
    const auto stargate = rig.framesFor(milliseconds(300));

    rig.click(visual.visualizerButton());
    TEST_CHECK(!visual.choice().on && !visual.active() && !visual.running());
    TEST_CHECK(rig.chosen.size() == 4 && !rig.chosen.back().on);
    TEST_CHECK(tooltipOf(visual.visualizerButton()) == "Turn on the audio visualizer");
    TEST_CHECK(!visual.visualButton().enabled());
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }, milliseconds(1000)));
    const auto glyph = rig.frame();
    TEST_CHECK(stageDiffering(stargate, glyph, rig.stageRows()) > 1000);

    rig.click(visual.visualizerButton());
    TEST_CHECK(visual.choice().on && visual.active() && visual.running());
    TEST_CHECK(rig.chosen.size() == 5 && rig.chosen.back().on);
    TEST_CHECK(!rig.view->showsSoundGlyph());
    // The owner applies a choice made elsewhere, such as in the settings, at once.
    visual.setChoice({.on = true, .kind = AudioVisualKind::Halo});
    TEST_CHECK(visual.choice().kind == AudioVisualKind::Halo && visual.running());
    TEST_CHECK(rig.chosen.size() == 5);
  }

  // Muted, the visual keeps moving. The visualizer was just turned on again, and a tap turned on while playing hears
  // its first window after the player's one-second audio queue and the output's buffer, so the check waits first.
  void movesWhileMuted(Rig& rig) {
    rig.click(rig.view->muteButton());
    TEST_CHECK(rig.view->player()->muted());
    (void)rig.framesFor(milliseconds(2500));
    const auto first = rig.frame();
    const auto later = rig.framesFor(milliseconds(400));
    std::println(
        "muted: {} stage pixels moved in 0.4 s; state {}, active {}, running {}, at {:.2f} s",
        stageDiffering(first, later, rig.stageRows()), static_cast<int>(rig.state()), rig.view->audioVisual().active(),
        rig.view->audioVisual().running(), rig.view->player()->position()
    );
    test::audio::keepIfAsked(later, "media-view-muted.png");
    TEST_CHECK(stageDiffering(first, later, rig.stageRows()) > 100);
    rig.click(rig.view->muteButton());
  }

  // Hidden, the player stops and the visual goes with its tap; nothing asks for a frame.
  void stopsWhenHidden(Rig& rig) {
    rig.view->setShown(false);
    TEST_CHECK(rig.view->player() == nullptr);
    TEST_CHECK(!rig.view->audioVisual().active() && !rig.view->audioVisual().running());
    (void)rig.frame();
    TEST_CHECK(!rig.asksForAFrame(milliseconds(300)));
    rig.view->setShown(true);
  }

  // Video is not affected: no buttons and no visual.
  void leavesVideoAlone(Rig& rig, const fs::path& video) {
    rig.view->setSource(video.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().video; }));
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.view->showsFrame(); }));
    TEST_CHECK(!rig.view->audioVisual().visualizerButton().visible());
    TEST_CHECK(!rig.view->audioVisual().visualButton().visible());
    TEST_CHECK(!rig.view->audioVisual().active() && !rig.view->audioVisual().running());
  }

  // After the end the glyph comes back.
  void givesTheStageBackAtTheEnd(Rig& rig, const fs::path& shortClip) {
    rig.view->setSource(shortClip.string());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.view->audioVisual().active(); }));
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Ended; }));
    TEST_CHECK(!rig.view->audioVisual().active() && !rig.view->audioVisual().running());
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }, milliseconds(1000)));
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "avidemux", "wavparse", "playbin", "appsink",
           "videoconvertscale", "fakesink", "identity"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  if (!luciole::MediaPlayer::available()) {
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
  const auto shortClip = scratch.path("short.wav");
  const auto video = scratch.path("clip.avi");
  test::audio::writeSoundClip(clip, test::audio::SoundClip::ToneAndKicks, 30);
  test::audio::writeSoundClip(shortClip, test::audio::SoundClip::ToneAndKicks, 2);
  TEST_CHECK(writeClip(video, {.width = 64, .height = 48, .seconds = 4}));
  {
    Rig rig(*capture);
    takesTheStageWhilePlaying(rig, clip);
    holdsWhilePaused(rig);
    buttonsChangeTheChoice(rig);
    movesWhileMuted(rig);
    stopsWhenHidden(rig);
    leavesVideoAlone(rig, video);
    givesTheStageBackAtTheEnd(rig, shortClip);
  }
  std::println("media-view-visual: all checks passed");
  return 0;
}
