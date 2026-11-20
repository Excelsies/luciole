#include "app/page/workbench_preview.h"
#include "app/page/workbench_session.h"
#include "audio_visual_clips.h"
#include "browser/workbench_services.h"
#include "core/input/key_modifiers.h"
#include "imaging/text_tile_service.h"
#include "media/media_player.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "silent_sound_output.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/controls/button.h"
#include "ui/controls/media_view.h"
#include "ui/workbench/column_preview_view.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <thread>
#include <vector>

// The sound visualizer's choice on its way between the page and a playing sound preview: the page's choice reaches
// the preview's media view at once, and the media view's buttons report the user's choice back to the page.
namespace {
  using luciole::AudioVisualKind;
  using luciole::MediaState;
  using luciole::WorkbenchPreview;
  using Clock = std::chrono::steady_clock;

  constexpr int kWidth = 1100;
  constexpr int kHeight = 700;

  struct Rig {
    luciole::test::TempFolder folder{"luciole-preview-visual"};
    ::test::SceneCapture& capture;
    luciole::WorkbenchServices services;
    luciole::TextTileService text;
    luciole::ThumbnailService thumbnails;
    luciole::FileIconCache icons;
    AnimationManager animations;
    luciole::BrowserController browser{{}, false};
    Node parent;
    Node* pane = nullptr;
    InputArea* focus = nullptr;
    std::unique_ptr<luciole::WorkbenchSession> session;
    std::unique_ptr<WorkbenchPreview> preview;
    InputDispatcher dispatcher;
    Clock::duration skew{};
    std::uint32_t serial = 0;
    std::uint32_t modifiers = 0;
    std::vector<AudioVisualChoice> chosen; // what the page was told the user chose
    std::vector<bool> loops;               // what the page was told the loop button chose

    explicit Rig(::test::SceneCapture& rig) : capture(rig) {
      test::audio::writeSoundClip(folder.path() / "song.wav", test::audio::SoundClip::ToneAndKicks, 30);
      parent.setAnimationManager(&animations);
      auto made = std::make_unique<Node>();
      made->setFlexGrow(1);
      focus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
      pane = parent.addChild(std::move(made));
      browser.open(luciole::Location::fromPath(folder.path().native()));
      until([this] { return browser.model().snapshot().status == luciole::DirectoryStatus::Ready; });
      session = std::make_unique<luciole::WorkbenchSession>(
          *pane, focus, services.measureCache(), icons, [] { return 0U; },
          luciole::WorkbenchModelOptions{.ancestorLevels = 0}
      );
      preview = std::make_unique<WorkbenchPreview>(
          session->previewSlot(), luciole::WorkbenchPreviewWorkers{.text = &text, .thumbnails = &thumbnails},
          luciole::MediaPlayer::available(),
          WorkbenchPreview::Hooks{
              .frameNeeded = {},
              .open = {},
              .overlay = {},
              .previewSizeChosen = {},
              .audioVisualChosen = [this](const AudioVisualChoice& choice) { chosen.push_back(choice); },
              .mediaLoopChosen = [this](bool on) { loops.push_back(on); },
              .modifiers = [this] { return modifiers; },
              .clock = [this] { return Clock::now() + skew; },
          }
      );
      dispatcher.setSceneRoot(&session->view());
    }
    ~Rig() {
      preview.reset();
      session.reset();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    static void pump() {
      for (int i = 0; i < 64 && g_main_context_iteration(nullptr, FALSE) != FALSE; ++i) {
      }
      TimerManager::instance().tick();
    }
    void until(const std::function<bool()>& done) {
      for (std::size_t round = 0; round < 4000 && !done(); ++round) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      TEST_CHECK(done());
    }
    // One page frame, as WorkbenchPreviewTest draws it.
    void frame() {
      pump();
      animations.tick(16);
      session->update(luciole::workbenchFeed(browser), luciole::WorkbenchChoices{});
      preview->update();
      (void)capture.capture(session->view(), kWidth, kHeight, 1);
      auto& slot = session->previewSlot();
      std::optional<LayoutRect> bounds;
      if (session->shows(slot) && slot.width() > 0 && slot.height() > 0) {
        LayoutRect at{.x = 0, .y = 0, .width = slot.width(), .height = slot.height()};
        Node::absolutePosition(&slot, at.x, at.y);
        bounds = luciole::previewBounds(at);
      }
      preview->place(*session, browser.model(), bounds, 1);
      thumbnails.endFrame();
    }
    bool frames(const std::function<bool()>& done) {
      const auto start = Clock::now();
      while (!done() && Clock::now() - start < std::chrono::seconds(10)) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
      }
      return done();
    }
    MediaView* media() const { return dynamic_cast<MediaView*>(session->previewSlot().content()); }
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
      dispatcher.pointerLeave();
      frame();
    }
    void open(std::string_view name) {
      auto& model = browser.model();
      for (const auto id : model.rows()) {
        if (model.entry(id)->name == name) {
          TEST_CHECK(model.select(id));
        }
      }
      frame();
      skew += WorkbenchPreview::kDwell;
      TEST_CHECK(frames([this] { return media() != nullptr && media()->facts().audio; }));
    }
  };

  void carriesTheChoiceBothWays(Rig& rig) {
    rig.preview->setAudioVisual({.on = true, .kind = AudioVisualKind::Orbit});
    rig.open("song.wav");
    auto* media = rig.media();
    TEST_CHECK((media->audioVisual().choice() == AudioVisualChoice{.on = true, .kind = AudioVisualKind::Orbit}));
    rig.click(media->playButton());
    TEST_CHECK(rig.frames([&] { return media->audioVisual().active(); }));

    // The page's choice reaches the playing file at once.
    rig.preview->setAudioVisual({.on = true, .kind = AudioVisualKind::Radar});
    TEST_CHECK(media->audioVisual().choice().kind == AudioVisualKind::Radar && media->audioVisual().running());
    TEST_CHECK(rig.chosen.empty());

    // The buttons tell the page.
    rig.click(media->audioVisual().visualizerButton());
    TEST_CHECK((rig.chosen.back() == AudioVisualChoice{.on = false, .kind = AudioVisualKind::Radar}));
    TEST_CHECK((rig.preview->audioVisual() == AudioVisualChoice{.on = false, .kind = AudioVisualKind::Radar}));
    TEST_CHECK(!media->audioVisual().active());
    rig.click(media->audioVisual().visualizerButton());
    rig.modifiers = KeyMod::Shift;
    rig.click(media->audioVisual().visualButton());
    rig.modifiers = 0;
    TEST_CHECK(rig.chosen.size() == 3);
    TEST_CHECK((rig.chosen.back() == AudioVisualChoice{.on = true, .kind = AudioVisualKind::Halo}));
    TEST_CHECK((rig.preview->audioVisual() == AudioVisualChoice{.on = true, .kind = AudioVisualKind::Halo}));
    TEST_CHECK(media->audioVisual().running());

    // The page's preview goes with the page; the next load starts from the choice the buttons made.
    rig.preview->hide();
    rig.frame();
    TEST_CHECK(rig.frames([&] { return rig.media() != nullptr; }));
    TEST_CHECK((rig.media()->audioVisual().choice() == AudioVisualChoice{.on = true, .kind = AudioVisualKind::Halo}));
  }

  void carriesTheLoopBothWays(Rig& rig) {
    // A saved choice is on the loop button of the next file, and on its player once it plays.
    rig.preview->setMediaLoop(true);
    rig.preview->hide();
    rig.frame();
    TEST_CHECK(rig.frames([&] { return rig.media() != nullptr && rig.media()->facts().audio; }));
    auto* media = rig.media();
    TEST_CHECK(media->loop() && rig.preview->mediaLoop());
    TEST_CHECK(media->loopButton().visible() && media->loopButton().width() > 0);
    rig.click(media->playButton());
    TEST_CHECK(rig.frames([&] {
      return media->player() != nullptr && media->player()->state() == MediaState::Playing;
    }));
    TEST_CHECK(media->player()->loop());

    // The page's choice reaches the playing file at once, without being told back.
    rig.preview->setMediaLoop(false);
    TEST_CHECK(!media->loop() && !media->player()->loop() && rig.loops.empty());

    // The button tells the page.
    rig.click(media->loopButton());
    TEST_CHECK((rig.loops == std::vector{true}) && rig.preview->mediaLoop() && media->player()->loop());
    rig.click(media->loopButton());
    TEST_CHECK((rig.loops == std::vector{true, false}) && !rig.preview->mediaLoop() && !media->player()->loop());
    rig.click(media->loopButton());
    TEST_CHECK((rig.loops == std::vector{true, false, true}));

    // The next load starts from what the button chose; the audio visualizer's choice stays as it was.
    const auto visual = rig.preview->audioVisual();
    rig.preview->hide();
    rig.frame();
    TEST_CHECK(rig.frames([&] { return rig.media() != nullptr; }));
    TEST_CHECK(rig.media()->loop());
    TEST_CHECK(rig.preview->audioVisual() == visual);
  }
} // namespace

int main() {
  const luciole::test::TempFolder gstreamer("luciole-preview-visual-gst");
  test::audio::useSilentSystemOutput(gstreamer.path());
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
  {
    Rig rig(*capture);
    carriesTheChoiceBothWays(rig);
    carriesTheLoopBothWays(rig);
  }
  std::println("workbench-preview-visual: all checks passed");
  return 0;
}
