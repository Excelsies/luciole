#include "media_clips.h"
#include "media_view_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <print>
#include <string>
#include <vector>

// The loop button of the media control bar: for video and sound, before and after Play, with its state shown, and
// followed by the player.
namespace {
  using luciole::MediaState;
  using namespace test::media;

  constexpr float kNarrowColumnText = 12;

  bool shown(const Button& button) { return button.visible() && button.participatesInLayout() && button.width() > 0; }

  void opens(ViewRig& rig, const fs::path& file) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
  }

  // The button sits beside Mute for a video and a sound file, before Play, and says whether loop is on.
  void showsTheButtonAndItsState(ViewRig& rig, const fs::path& clip, const fs::path& tone) {
    for (const auto& file : {clip, tone}) {
      opens(rig, file);
      auto& loop = rig.view->loopButton();
      auto& mute = rig.view->muteButton();
      TEST_CHECK(rig.view->player() == nullptr);
      TEST_CHECK(shown(loop) && shown(mute));
      TEST_CHECK(loop.parent() == mute.parent());
      float loopX = 0;
      float muteX = 0;
      float y = 0;
      Node::absolutePosition(&loop, loopX, y);
      Node::absolutePosition(&mute, muteX, y);
      TEST_CHECK(loopX + loop.width() <= muteX + 0.5F && muteX - (loopX + loop.width()) < 16);
      TEST_CHECK(!rig.view->loop());
      TEST_CHECK(tooltipOf(loop) == "Turn on looping");
      const auto off = settled(rig);

      std::vector<bool> heard;
      rig.view->onLoopChosen = [&heard](bool on) { heard.push_back(on); };
      rig.click(loop);
      const auto on = settled(rig);
      TEST_CHECK((heard == std::vector{true}));
      TEST_CHECK(rig.view->loop());
      TEST_CHECK(tooltipOf(loop) == "Turn off looping");
      TEST_CHECK(differing(off, on, loop) > 10);
      rig.click(loop);
      TEST_CHECK((heard == std::vector{true, false}));
      TEST_CHECK(!rig.view->loop() && tooltipOf(loop) == "Turn on looping");
      TEST_CHECK(differing(off, settled(rig), loop) == 0);

      // The owner's choice shows the same without telling the owner back.
      rig.view->setLoop(true);
      TEST_CHECK(rig.view->loop() && tooltipOf(loop) == "Turn off looping" && heard.size() == 2);
      rig.view->setLoop(false);
      TEST_CHECK(!rig.view->loop() && tooltipOf(loop) == "Turn on looping" && heard.size() == 2);
      rig.view->onLoopChosen = {};
    }
  }

  // The player follows the switch from its start and at once while it plays.
  void thePlayerFollowsTheSwitch(ViewRig& rig, const fs::path& clip, const fs::path& tone) {
    opens(rig, clip);
    rig.click(rig.view->playButton());
    rig.playing();
    TEST_CHECK(!rig.view->player()->loop());
    rig.click(rig.view->loopButton());
    TEST_CHECK(rig.view->player()->loop());
    rig.click(rig.view->loopButton());
    TEST_CHECK(!rig.view->player()->loop());
    // A choice made before Play is the player's from its start, for the next file as well.
    rig.view->setLoop(true);
    opens(rig, tone);
    TEST_CHECK(rig.view->player() == nullptr);
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.view->player() != nullptr && rig.view->player()->loop());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    rig.view->setLoop(false);
    TEST_CHECK(!rig.view->player()->loop());
  }

  // A sound file that reaches its end plays on while the button is on, and ends once it is turned off.
  void aClickLoopsTheFileAndAClickEndsIt(ViewRig& rig, const fs::path& tone) {
    opens(rig, tone);
    rig.view->setLoop(false);
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && rig.position() > 0.3; }));
    rig.click(rig.view->loopButton());
    int wraps = 0;
    bool late = false;
    TEST_CHECK(rig.until([&] {
      const double at = rig.position();
      if (at > 0.6) {
        late = true;
      } else if (late && at < 0.4) {
        late = false;
        ++wraps;
      }
      TEST_CHECK(rig.state() == MediaState::Playing);
      return wraps >= 2;
    }));
    TEST_CHECK(tooltipOf(rig.view->playButton()) == "Pause");
    rig.click(rig.view->loopButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Ended; }));
  }

  // Every control of the bar that shows lies inside the view, in order, and the position bar keeps room.
  void expectBarFits(ViewRig& rig, float least) {
    std::vector<const Node*> parts{
        &rig.view->playButton(), &rig.view->positionBar(), &rig.view->loopButton(), &rig.view->muteButton()
    };
    for (const Node* part :
         {static_cast<const Node*>(&rig.view->elapsedLabel()), static_cast<const Node*>(&rig.view->totalLabel()),
          static_cast<const Node*>(&rig.view->audioVisual().visualizerButton()),
          static_cast<const Node*>(&rig.view->audioVisual().visualButton())}) {
      if (part->visible() && part->participatesInLayout()) {
        parts.push_back(part);
      }
    }
    float right = 0;
    float left = 1e9F;
    for (const Node* part : parts) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(part, x, y);
      TEST_CHECK(part->width() > 0);
      TEST_CHECK(x >= -0.5F && x + part->width() <= static_cast<float>(rig.width) + 0.5F);
      left = std::min(left, x);
      right = std::max(right, x + part->width());
    }
    TEST_CHECK(left >= 0 && right <= static_cast<float>(rig.width) + 0.5F);
    TEST_CHECK(rig.view->positionBar().width() >= least);
  }

  // No point of the row's height inside the view belongs to `button`.
  bool cannotBeHit(ViewRig& rig, const Button& button) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&rig.view->loopButton(), x, y);
    const float row = y + rig.view->loopButton().height() / 2;
    for (int at = 0; at < rig.width; ++at) {
      for (const Node* hit = Node::hitTest(&rig.root, static_cast<float>(at) + 0.5F, row); hit != nullptr;
           hit = hit->parent()) {
        if (hit == &button) {
          return false;
        }
      }
    }
    return true;
  }

  // At the narrowest column (140, the view gets all of it: the column puts no padding around its content) a video
  // fits the bar, the time giving way to the position bar and the loop button. A sound file there also gives up the
  // visual button, which returns, as the times do, when there is room. A sound file with its two visualizer buttons
  // fits from 200 with no time to spare.
  void theBarFitsTheNarrowColumns(ViewRig& rig, const fs::path& clip, const fs::path& tone) {
    rig.width = 140;
    opens(rig, clip);
    TEST_CHECK(rig.until([&] { return !rig.view->elapsedLabel().visible(); }, std::chrono::milliseconds(1000)));
    expectBarFits(rig, 24);
    std::println("video at 140: the position bar is {:.0f} wide", rig.view->positionBar().width());
    rig.width = kWidth;
    TEST_CHECK(rig.until([&] { return rig.view->elapsedLabel().visible() && rig.view->totalLabel().visible(); }));
    expectBarFits(rig, 24);
    auto& next = rig.view->audioVisual().visualButton();
    for (const int width : {200, 240}) {
      rig.width = width;
      opens(rig, tone);
      rig.frame();
      TEST_CHECK(rig.view->audioVisual().visualizerButton().visible());
      TEST_CHECK(next.visible() && next.participatesInLayout());
      expectBarFits(rig, 24);
      std::println("sound at {}: the position bar is {:.0f} wide", width, rig.view->positionBar().width());
    }
    // The preview's text in the third column, the one 140 wide, is 12 with the list's default 14.
    rig.view->setTextSize(kNarrowColumnText);
    rig.width = 140;
    rig.frame();
    TEST_CHECK(!rig.view->elapsedLabel().visible() && !rig.view->totalLabel().visible());
    TEST_CHECK(rig.view->audioVisual().visualizerButton().visible());
    TEST_CHECK(!next.visible() && !next.participatesInLayout());
    TEST_CHECK(cannotBeHit(rig, next));
    expectBarFits(rig, 24);
    std::println("sound at 140: the position bar is {:.0f} wide", rig.view->positionBar().width());
    // A change of the visual's own state does not bring the button back.
    rig.view->audioVisual().setChoice({.on = false, .kind = luciole::AudioVisualKind::Pulsar});
    rig.frame();
    TEST_CHECK(!next.visible() && cannotBeHit(rig, next));
    expectBarFits(rig, 24);
    // Larger text leaves the bar less room, and every control still lies inside the view.
    rig.view->setTextSize(Style::fontSizeCaption);
    rig.frame();
    TEST_CHECK(!next.visible());
    expectBarFits(rig, 1);
    rig.width = 240;
    rig.frame();
    TEST_CHECK(next.visible() && next.participatesInLayout());
    expectBarFits(rig, 24);
    rig.width = kWidth;
    rig.frame();
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
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 12}));
  writeTone(tone, 1);
  {
    ViewRig rig(*capture);
    showsTheButtonAndItsState(rig, clip, tone);
    theBarFitsTheNarrowColumns(rig, clip, tone);
    thePlayerFollowsTheSwitch(rig, clip, tone);
  }
  {
    ViewRig rig(*capture);
    aClickLoopsTheFileAndAClickEndsIt(rig, tone);
  }
  std::println("media-view-loop: all checks passed");
  return 0;
}
