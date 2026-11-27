#include "media_clips.h"
#include "media_view_rig.h"
#include "render/animation/animation_manager.h"
#include "render/core/image_decoder.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/browser/preview_layout.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"

#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <utility>
#include <variant>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using luciole::AudioOutput;
  using luciole::MediaState;
  using namespace test::media;

  // Before Play the view reads only the container: its playing time and picture size, with nothing decoded.
  void readsFactsBeforePlay(ViewRig& rig, const fs::path& clip) {
    int heard = 0;
    rig.view->onFactsChanged = [&heard] { ++heard; };
    rig.view->setSource(clip.string());
    rig.view->setPoster(solidPoster(64, 48));
    TEST_CHECK(rig.until([&] { return rig.view->showsPoster(); }, std::chrono::milliseconds(2000)));
    TEST_CHECK(rig.until([&] { return rig.view->totalLabel().text() == "0:12"; }));
    TEST_CHECK(heard == 1);
    const auto& facts = rig.view->facts();
    TEST_NEAR(facts.seconds, 12.0, 0.05);
    TEST_CHECK(facts.video && facts.width == 64 && facts.height == 48);
    TEST_CHECK(rig.view->player() == nullptr);
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
    TEST_CHECK(rig.view->elapsedLabel().text() == "0:00");
    TEST_CHECK(!rig.view->showsSoundGlyph());
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Play");
    rig.view->onFactsChanged = {};
  }

  void playsAndShowsTime(ViewRig& rig, const fs::path& clip) {
    rig.click(rig.view->playButton());
    rig.playing();
    TEST_CHECK(holdsOpen(clip));
    TEST_CHECK(!rig.view->showsPoster());
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Pause");
    TEST_CHECK(rig.view->totalLabel().text() == "0:12");
    // The clock follows playback four times a second: over a second and a half of playing the bar moves at
    // least four times, and the time reads the second the player is in.
    const int due = TimerManager::instance().pollTimeoutMs();
    TEST_CHECK(due >= 0 && due <= MediaView::kClockInterval.count());
    const double from = rig.position();
    double lastFraction = rig.view->positionBar().fraction();
    int moves = 0;
    TEST_CHECK(rig.until([&] {
      if (rig.view->positionBar().fraction() != lastFraction) {
        lastFraction = rig.view->positionBar().fraction();
        ++moves;
      }
      return rig.position() >= from + 1.5;
    }));
    std::println("the bar moved {} times over 1.5 s of playing", moves);
    TEST_CHECK(moves >= 4);
    TEST_CHECK(rig.until([&] { return rig.position() >= 1.5; }));
    TEST_CHECK(rig.until([&] { return rig.view->elapsedLabel().text() != "0:00"; }, std::chrono::milliseconds(600)));
  }

  void togglesAndSeeksByKey(ViewRig& rig) {
    // A click on the picture toggles and takes the keyboard.
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(rig.view->stageArea().focused());
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.state() == MediaState::Playing);
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.state() == MediaState::Paused);
    // Paused, the clock stops: the nearest timer is the wait that gives the pipeline back, which lies a whole
    // kIdleRelease ahead (a repeating clock or a seek watch would be due within a second).
    TEST_CHECK(rig.until(
        [] {
          const int due = TimerManager::instance().pollTimeoutMs();
          return due > (MediaView::kIdleRelease - std::chrono::milliseconds(5000)).count()
              && due <= MediaView::kIdleRelease.count();
        },
        std::chrono::milliseconds(2000)
    ));
    const double at = rig.position();
    rig.key(XKB_KEY_Right);
    TEST_NEAR(rig.position(), at + MediaView::kSeekStep, 0.15);
    rig.key(XKB_KEY_Left);
    TEST_NEAR(rig.position(), at, 0.15);
    rig.key(XKB_KEY_Left);
    TEST_NEAR(rig.position(), 0.0, 0.15);
  }

  // One click on the picture, before the first Play or after it, plays or pauses and takes the keyboard; Space
  // then toggles again.
  void clickOnThePictureStartsPlayback(ViewRig& rig, const fs::path& clip) {
    rig.view->setSource(clip.string());
    rig.view->setPoster(solidPoster(32, 24));
    TEST_CHECK(rig.until([&] { return rig.view->showsPoster(); }));
    TEST_CHECK(rig.view->player() == nullptr);
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.view->player() != nullptr);
    rig.playing();
    TEST_CHECK(rig.view->stageArea().focused());
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Pause");
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Play");
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.state() == MediaState::Playing);
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.state() == MediaState::Paused);
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.state() == MediaState::Playing);
  }

  void seeksWithTheBar(ViewRig& rig) {
    TEST_CHECK(rig.state() == MediaState::Paused);
    rig.click(rig.view->positionBar(), 0.5F);
    TEST_NEAR(rig.position(), 6.0, 0.3);
    TEST_CHECK(rig.until([&] { return rig.view->elapsedLabel().text() == "0:06"; }));
    // A drag seeks along the way and lands where it is released.
    const auto& bar = rig.view->positionBar();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    rig.press(x + bar.width() * 0.25F, y + bar.height() / 2);
    TEST_NEAR(rig.position(), 3.0, 0.3);
    rig.dispatcher.pointerMotion(x + bar.width() * 0.5F, y + bar.height() / 2, ++rig.serial);
    rig.frame();
    rig.dispatcher.pointerMotion(x + bar.width() * 0.75F, y + bar.height() / 2, ++rig.serial);
    rig.release(x + bar.width() * 0.75F, y + bar.height() / 2);
    TEST_NEAR(rig.position(), 9.0, 0.3);
    TEST_CHECK(rig.until([&] { return rig.view->elapsedLabel().text() == "0:09"; }));
  }

  // A drag the compositor cancels, as when the pointer leaves the window, ends there: the bar and the time then
  // follow playback.
  void followsPlaybackAfterACancelledDrag(ViewRig& rig) {
    TEST_CHECK(rig.state() == MediaState::Paused);
    const auto& bar = rig.view->positionBar();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&bar, x, y);
    rig.press(x + bar.width() * 0.25F, y + bar.height() / 2);
    TEST_NEAR(rig.position(), 3.0, 0.3);
    rig.dispatcher.pointerLeave();
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && rig.position() > 4.2; }));
    // The clock catches up within its interval: the time leaves the second the drag ended in.
    TEST_CHECK(rig.until([&] { return rig.view->elapsedLabel().text() != "0:03"; }, std::chrono::milliseconds(1000)));
    // The bar trails playback by at most one interval of the clock, with room for a late turn.
    TEST_NEAR(bar.fraction(), rig.position() / 12.0, 0.5 / 12.0);
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.state() == MediaState::Paused);
  }

  // A poster given to a view that is hidden before it was ever laid out waits without a timer.
  void hiddenPosterCostsNothing(::test::SceneCapture& capture) {
    AnimationManager animations;
    Flex root;
    root.setDirection(FlexDirection::Vertical);
    root.setAnimationManager(&animations);
    auto* view = static_cast<MediaView*>(root.addChild(std::make_unique<MediaView>(ColorRole::SurfaceContainerLow, AudioOutput::Silent)));
    view->setFlexGrow(1);
    view->setVisible(false);
    view->setPoster(solidPoster(4, 4));
    Pump pump;
    const auto frame = [&] {
      pump.once();
      animations.tick(16);
      (void)capture.capture(root, kWidth, kHeight, 1);
    };
    bool idle = false;
    for (int i = 0; i < 400 && !idle; ++i) {
      frame();
      idle = !animations.hasActive();
    }
    TEST_CHECK(idle);
    for (int i = 0; i < 10; ++i) {
      frame();
    }
    TEST_CHECK(!animations.hasActive());
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 0);
    TEST_CHECK(!view->showsPoster());
    view->setVisible(true);
    for (int i = 0; i < 10 && !view->showsPoster(); ++i) {
      frame();
    }
    TEST_CHECK(view->showsPoster());
  }

  void mutes(ViewRig& rig) {
    TEST_CHECK(tooltipOf(rig.view->muteButton()) == "Mute");
    rig.click(rig.view->muteButton());
    TEST_CHECK(rig.view->player()->muted());
    TEST_CHECK(tooltipOf(rig.view->muteButton()) == "Unmute");
  }

  void stopsWhenHidden(ViewRig& rig, const fs::path& clip) {
    rig.view->setShown(false);
    TEST_CHECK(rig.view->player() == nullptr);
    TEST_CHECK(!rig.view->showsFrame() && !rig.view->showsPoster());
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
    rig.view->setShown(true);
    TEST_CHECK(rig.until([&] { return rig.view->showsPoster(); }, std::chrono::milliseconds(2000)));
    // Played again, it keeps the sound muted.
    rig.click(rig.view->playButton());
    rig.playing();
    TEST_CHECK(rig.view->player()->muted());
    // A hidden ancestor stops it as well.
    rig.middle->setVisible(false);
    TEST_CHECK(rig.until([&] { return rig.view->player() == nullptr; }, std::chrono::milliseconds(2000)));
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
    rig.middle->setVisible(true);
  }

  void stopsWhenTheFileChanges(ViewRig& rig, const fs::path& clip, const fs::path& tone) {
    rig.click(rig.view->playButton());
    rig.playing();
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.view->player() == nullptr);
    TEST_CHECK(!rig.view->showsPoster()); // the poster belonged to the other file
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
    // Sound only and no cover: the facts arrive and the sound glyph fills the stage.
    TEST_CHECK(rig.until([&] { return rig.view->totalLabel().text() == "0:02"; }));
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }, std::chrono::milliseconds(1000)));
    TEST_CHECK(rig.view->facts().audio && !rig.view->facts().video);
    // The time runs, no frame shows and the sound's visual takes the glyph's place.
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && rig.position() > 0.2; }));
    TEST_CHECK(!rig.view->showsFrame());
    TEST_CHECK(rig.view->audioVisual().active() && !rig.view->showsSoundGlyph());
    // A cover from the owner takes the glyph's place.
    rig.view->setPoster(solidPoster(32, 32));
    TEST_CHECK(rig.until([&] { return rig.view->showsPoster(); }, std::chrono::milliseconds(1000)));
    TEST_CHECK(!rig.view->showsSoundGlyph());
  }

  // Resized while paused, the view shows the frame where it stands at the new size, not the old one stretched.
  void refitsThePausedFrame(ViewRig& rig) {
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_CHECK(rig.until([&] { return rig.view->framePixels() == std::pair(64, 48); }));
    const double at = rig.position();
    rig.width = 80;
    rig.height = 60;
    rig.frame();
    // The picture is seen in the stage less room for its shadow on each side: 12 pixels, or a quarter of a small
    // stage's shorter side.
    const float room = std::min(
        luciole::kPreviewImageInset, std::min(rig.view->stageArea().width(), rig.view->stageArea().height()) / 4
    );
    const auto stage = static_cast<int>(std::lround(rig.view->stageArea().height() - (2 * room)));
    TEST_CHECK(stage > 0 && stage < 48);
    // The 4:3 frame now fills that box's height.
    TEST_CHECK(rig.until([&] { return rig.view->framePixels().second == stage; }));
    TEST_CHECK(std::abs(rig.view->framePixels().first * 3 - stage * 4) <= 3);
    TEST_CHECK(rig.state() == MediaState::Paused);
    TEST_NEAR(rig.position(), at, 0.11);
    rig.width = kWidth;
    rig.height = kHeight;
    TEST_CHECK(rig.until([&] { return rig.view->framePixels() == std::pair(64, 48); }));
  }

  // A file chosen while the facts of the one before are still being read gets its own facts.
  void readsTheFactsOfTheLatestSource(ViewRig& rig, const fs::path& clip, const fs::path& tone) {
    rig.view->setSource(clip.string());
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    const auto& facts = rig.view->facts();
    TEST_NEAR(facts.seconds, 2.0, 0.05);
    TEST_CHECK(facts.audio && !facts.video);
    TEST_CHECK(facts.width == 0 && facts.height == 0);
    TEST_CHECK(rig.view->totalLabel().text() == "0:02");
    TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }, std::chrono::milliseconds(1000)));
  }

  // Sound that reaches the reader through a parser, as FLAC and MP3 do, shows its time and the sound glyph before
  // Play.
  void showsTheSoundGlyphForParsedSound(ViewRig& rig, const Scratch& scratch) {
    for (const auto& [name, encoder] : {std::pair("parsed.flac", "flacenc"), std::pair("parsed.mp3", "lamemp3enc")}) {
      const auto path = scratch.path(name);
      TEST_CHECK(writeSound(path, encoder));
      rig.view->setSource(path.string());
      TEST_CHECK(rig.until([&] { return rig.view->totalLabel().text() == "0:02"; }));
      TEST_CHECK(rig.until([&] { return rig.view->showsSoundGlyph(); }, std::chrono::milliseconds(1000)));
      TEST_CHECK(rig.view->facts().audio && !rig.view->facts().video);
      TEST_CHECK(rig.view->player() == nullptr);
    }
  }

  void reportsErrors(ViewRig& rig, const fs::path& broken) {
    rig.view->setSource(broken.string());
    TEST_CHECK(rig.view->messageLabel().text().empty());
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return !rig.view->messageLabel().text().empty(); }));
    std::println("message: {}", rig.view->messageLabel().text());
    TEST_CHECK(rig.until([&] { return !holdsOpen(broken); }));
  }

  void freesWhenDestroyed(ViewRig& rig, const fs::path& clip) {
    rig.view->setSource(clip.string());
    rig.view->setTextSize(11);
    TEST_NEAR(rig.view->elapsedLabel().fontSize(), 11.0F, 0.01F);
    rig.click(rig.view->playButton());
    rig.playing();
    (void)rig.middle->removeChild(rig.view);
    rig.view = nullptr;
    TEST_CHECK(rig.until([&] { return !holdsOpen(clip); }));
  }
} // namespace

int main() {
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "playbin",
           "appsink", "videoconvertscale", "fakesink"}
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
  const auto clip = scratch.path("clip.avi");
  const auto tone = scratch.path("tone.wav");
  const auto broken = scratch.path("broken.avi");
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 12}));
  writeTone(tone, 2);
  writeBytes(broken, "not a movie");
  {
    ViewRig rig(*capture);
    clickOnThePictureStartsPlayback(rig, clip);
  }
  {
    ViewRig rig(*capture);
    readsFactsBeforePlay(rig, clip);
    playsAndShowsTime(rig, clip);
    togglesAndSeeksByKey(rig);
    seeksWithTheBar(rig);
    followsPlaybackAfterACancelledDrag(rig);
    refitsThePausedFrame(rig);
    mutes(rig);
    stopsWhenHidden(rig, clip);
    stopsWhenTheFileChanges(rig, clip, tone);
    reportsErrors(rig, broken);
    freesWhenDestroyed(rig, clip);
  }
  {
    ViewRig rig(*capture);
    readsTheFactsOfTheLatestSource(rig, clip, tone);
    if (const auto lacking = missingElements(kParsedClipElements); lacking.empty()) {
      showsTheSoundGlyphForParsedSound(rig, scratch);
    } else {
      std::println("parsed sound not checked: GStreamer lacks {}", lacking);
    }
  }
  hiddenPosterCostsNothing(*capture);
  std::println("media-view: all checks passed");
  return 0;
}
