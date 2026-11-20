#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "media_clips.h"
#include "media_view_rig.h"
#include "render/scene/input_area.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/audio_visual_view.h"
#include "ui/controls/button.h"
#include "ui/controls/media_position_bar.h"
#include "ui/controls/media_view.h"
#include "ui/palette_file.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <print>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The keyboard of the media view after a pointer click: on the picture and on every control of the bar, Space plays
// and pauses and Left and Right seek; with Tab the bar's buttons stay reachable and Enter or Space press them.
namespace {
  using luciole::MediaState;
  using namespace test::media;

  constexpr auto kSettle = std::chrono::milliseconds(3000);

  // A click on a control, and what the click must have produced once it did its job.
  struct Control {
    std::string name;
    std::function<std::function<bool()>(ViewRig&)> click;
  };

  std::vector<Control> controls(const MediaView& view, bool sound) {
    std::vector<Control> list{
        {"Play",
         [&view](ViewRig& rig) {
           // Playing when clicked, so the click pauses.
           rig.click(view.playButton());
           return [&rig] { return rig.state() == MediaState::Paused; };
         }},
        {"position bar",
         [&view](ViewRig& rig) {
           rig.click(view.positionBar(), 0.25F);
           const double target = view.facts().seconds * 0.25;
           return [&rig, target] { return std::abs(rig.position() - target) < 2.0; };
         }},
        {"loop",
         [&view](ViewRig& rig) {
           const bool was = view.loop();
           rig.click(view.loopButton());
           return [&view, was] { return view.loop() == !was; };
         }},
        {"Mute", [&view](ViewRig& rig) {
           const bool was = view.player()->muted();
           rig.click(view.muteButton());
           return [&view, was] { return view.player()->muted() == !was; };
         }},
    };
    if (sound) {
      list.push_back({"visualizer", [&view](ViewRig& rig) {
                        const bool was = view.audioVisual().choice().on;
                        rig.click(view.audioVisual().visualizerButton());
                        return [&view, was] { return view.audioVisual().choice().on == !was; };
                      }});
      list.push_back({"visual", [&view](ViewRig& rig) {
                        // The visual button works while the visualizer is on.
                        auto choice = view.audioVisual().choice();
                        choice.on = true;
                        view.audioVisual().setChoice(choice);
                        rig.frame();
                        const auto was = view.audioVisual().choice().kind;
                        rig.click(view.audioVisual().visualButton());
                        return [&view, was] { return view.audioVisual().choice().kind != was; };
                      }});
    }
    return list;
  }

  // Plays the file, with the picture holding the keyboard.
  void startPlaying(ViewRig& rig) {
    if (rig.state() != MediaState::Playing) {
      rig.click(rig.view->stageArea());
    }
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && rig.position() > 0.1; }));
  }

  void pause(ViewRig& rig) {
    if (rig.state() == MediaState::Playing) {
      rig.key(XKB_KEY_space);
    }
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused; }, kSettle));
  }

  // After a click on the control: Space plays and pauses, Left and Right step five seconds.
  void keysWorkAfterAClickOn(ViewRig& rig, const Control& control) {
    std::println("after a click on {}", control.name);
    startPlaying(rig);
    const auto produced = control.click(rig);
    TEST_CHECK(rig.until(produced, kSettle));
    // Whatever the click did, Space reaches the player: it flips the play state and flips it back.
    const auto clicked = rig.state();
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() != clicked; }, kSettle));
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == clicked; }, kSettle));
    pause(rig);
    const double at = rig.position();
    rig.key(XKB_KEY_Right);
    TEST_NEAR(rig.position(), at + MediaView::kSeekStep, 0.3);
    rig.key(XKB_KEY_Left);
    TEST_NEAR(rig.position(), at, 0.3);
    // The next control starts from the same state.
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }, kSettle));
  }

  void everyControlKeepsTheKeys(ViewRig& rig, const fs::path& file, bool sound) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    if (sound) {
      TEST_CHECK(rig.view->audioVisual().visualizerButton().visible());
      TEST_CHECK(rig.view->audioVisual().visualButton().visible());
    }
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    for (const auto& control : controls(*rig.view, sound)) {
      keysWorkAfterAClickOn(rig, control);
    }
  }

  // A click on the control that starts playback takes the keys before anything else did.
  void thePlayButtonAsTheFirstClick(ViewRig& rig, const fs::path& file) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.click(rig.view->playButton());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing && rig.position() > 0.1; }));
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused; }, kSettle));
  }

  // With Tab the picture comes first, then the buttons in the bar's order; Space and Enter press the one focused.
  void tabReachesTheButtons(ViewRig& rig, const fs::path& file) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    rig.click(rig.view->stageArea());
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }));
    TEST_CHECK(rig.view->stageArea().focused());
    rig.key(XKB_KEY_Tab); // Play
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused; }, kSettle));
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }, kSettle));
    rig.key(XKB_KEY_Tab); // loop
    TEST_CHECK(!rig.view->loop());
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.view->loop());
    rig.key(XKB_KEY_Return);
    TEST_CHECK(!rig.view->loop());
    TEST_CHECK(rig.state() == MediaState::Playing);
    rig.key(XKB_KEY_Tab); // Mute
    TEST_CHECK(!rig.view->player()->muted());
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.view->player()->muted());
    rig.key(XKB_KEY_Return);
    TEST_CHECK(!rig.view->player()->muted());
    rig.key(XKB_KEY_Tab); // around to the picture
    TEST_CHECK(rig.view->stageArea().focused());
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused; }, kSettle));
  }

  // A sound file's bar has the two visualizer buttons between Play and loop in the Tab order.
  void tabReachesTheVisualizerButtons(ViewRig& rig, const fs::path& tone) {
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    rig.click(rig.view->stageArea());
    rig.key(XKB_KEY_Tab); // Play
    rig.key(XKB_KEY_Tab); // the visualizer's button
    const auto before = rig.view->audioVisual().choice();
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.view->audioVisual().choice().on != before.on);
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.view->audioVisual().choice().on == before.on);
    rig.key(XKB_KEY_Tab); // the visual's button
    rig.key(XKB_KEY_Return);
    TEST_CHECK(rig.view->audioVisual().choice().kind != before.kind);
    rig.key(XKB_KEY_Tab); // loop
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.view->loop());
    rig.key(XKB_KEY_Return);
    TEST_CHECK(!rig.view->loop());
  }
  // The visual's button is disabled while the visualizer is off, and Tab skips it.
  void tabSkipsTheDisabledVisualButton(ViewRig& rig, const fs::path& tone) {
    rig.view->setSource(tone.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    rig.click(rig.view->stageArea());
    rig.key(XKB_KEY_Tab); // Play
    rig.key(XKB_KEY_Tab); // the visualizer's button
    rig.key(XKB_KEY_space);
    TEST_CHECK(!rig.view->audioVisual().choice().on);
    TEST_CHECK(!rig.view->audioVisual().visualButton().enabled());
    rig.key(XKB_KEY_Tab); // loop
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.view->loop());
    TEST_CHECK(!rig.view->audioVisual().choice().on);
  }

  struct Spot {
    float x = 0;
    float y = 0;
  };

  Spot middleOf(const Node& node) {
    Spot spot;
    Node::absolutePosition(&node, spot.x, spot.y);
    spot.x += node.width() / 2;
    spot.y += node.height() / 2;
    return spot;
  }

  // The buttons of the bar that `sound` brings.
  std::vector<Button*> barButtons(MediaView& view, bool sound) {
    std::vector<Button*> buttons{&view.playButton(), &view.loopButton(), &view.muteButton()};
    if (sound) {
      buttons.push_back(&view.audioVisual().visualizerButton());
    }
    return buttons;
  }

  // A button held down shows its pressed look, which is not its hover look. Released in place it activates and, put
  // back, looks as it did before; dragged off and released elsewhere it does nothing and looks as it did before.
  void theBarShowsItsPressedLook(ViewRig& rig, const fs::path& file, bool sound) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    for (auto* button : barButtons(*rig.view, sound)) {
      const auto at = middleOf(*button);
      const auto rest = settled(rig);
      rig.dispatcher.pointerEnter(at.x, at.y, ++rig.serial);
      const auto hovered = drawn(rig);
      TEST_CHECK(differing(rest, hovered, *button) > 10);
      rig.dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, ++rig.serial, 1000, false);
      const auto held = drawn(rig);
      TEST_CHECK(differing(hovered, held, *button) > 10);
      // Dragged off the button and released over the picture.
      const auto stage = middleOf(rig.view->stageArea());
      rig.dispatcher.pointerMotion(stage.x, stage.y, ++rig.serial);
      rig.release(stage.x, stage.y);
      TEST_CHECK(differing(rest, settled(rig), *button) == 0);
      TEST_CHECK(!rig.view->loop() && rig.view->player() == nullptr);
    }
    // Released in place.
    auto& loop = rig.view->loopButton();
    const auto at = middleOf(loop);
    const auto rest = settled(rig);
    rig.press(at.x, at.y);
    rig.release(at.x, at.y);
    TEST_CHECK(rig.view->loop());
    rig.view->setLoop(false);
    TEST_CHECK(differing(rest, settled(rig), loop) == 0);
  }

  // The tooltip reached through a button's rectangle is the button's own.
  void theTooltipIsTheButtons(ViewRig& rig, const fs::path& file, bool sound) {
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    rig.frame();
    for (auto* button : barButtons(*rig.view, sound)) {
      const auto at = middleOf(*button);
      const InputArea* area = nullptr;
      for (const Node* hit = Node::hitTest(&rig.root, at.x, at.y); hit != nullptr && area == nullptr;
           hit = hit->parent()) {
        area = dynamic_cast<const InputArea*>(hit);
      }
      TEST_CHECK(area != nullptr);
      const auto content = area->tooltipContent();
      const auto* text = std::get_if<std::string>(&content);
      TEST_CHECK(text != nullptr && !text->empty() && *text == tooltipOf(*button));
    }
  }

  // Where the time labels are hidden, one click on loop still turns it on and the keys still reach the player.
  void loopAtTheNarrowestColumnKeepsTheKeys(ViewRig& rig, const fs::path& file) {
    rig.width = 140;
    rig.view->setSource(file.string());
    TEST_CHECK(rig.until([&] { return rig.view->facts().seconds > 0; }));
    TEST_CHECK(rig.until([&] { return !rig.view->elapsedLabel().visible(); }, std::chrono::milliseconds(1000)));
    startPlaying(rig);
    TEST_CHECK(!rig.view->loop());
    rig.click(rig.view->loopButton());
    TEST_CHECK(rig.until([&] { return rig.view->loop(); }, kSettle));
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Paused; }, kSettle));
    rig.key(XKB_KEY_space);
    TEST_CHECK(rig.until([&] { return rig.state() == MediaState::Playing; }, kSettle));
  }
} // namespace

int main() {
  // The keys the application binds at startup: Tab moves the focus, Enter and Space press the focused button.
  KeybindMatcher::setMatcher(KeybindAction::TabNext, [](std::uint32_t key, std::uint32_t modifiers) {
    return key == XKB_KEY_Tab && modifiers == 0;
  });
  KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t modifiers) {
    return modifiers == 0 && KeySymbol::isEnterOrSpace(key);
  });
  setPalette(luciole::defaultPalette()); // hover and pressed take different colors
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
  TEST_CHECK(writeClip(clip, {.width = 64, .height = 48, .seconds = 40}));
  writeTone(tone, 40);
  {
    ViewRig rig(*capture);
    everyControlKeepsTheKeys(rig, clip, false);
  }
  {
    ViewRig rig(*capture);
    everyControlKeepsTheKeys(rig, tone, true);
  }
  {
    ViewRig rig(*capture);
    thePlayButtonAsTheFirstClick(rig, clip);
  }
  {
    ViewRig rig(*capture);
    tabReachesTheButtons(rig, clip);
  }
  {
    ViewRig rig(*capture);
    tabReachesTheVisualizerButtons(rig, tone);
  }
  {
    ViewRig rig(*capture);
    tabSkipsTheDisabledVisualButton(rig, tone);
  }
  for (const auto& [file, sound] : {std::pair{clip, false}, std::pair{tone, true}}) {
    ViewRig pressed(*capture);
    theBarShowsItsPressedLook(pressed, file, sound);
    ViewRig tips(*capture);
    theTooltipIsTheButtons(tips, file, sound);
  }
  {
    ViewRig rig(*capture);
    loopAtTheNarrowestColumnKeepsTheKeys(rig, clip);
  }
  std::println("media-view-keys: all checks passed");
  return 0;
}
