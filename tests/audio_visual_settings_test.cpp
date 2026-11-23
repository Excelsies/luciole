#include "audio_visual_clips.h"
#include "config/settings.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/select_popup_context.h"
#include "ui/controls/toggle.h"
#include "ui/dialogs/settings_dialog.h"

#include <cstdint>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// The sound visualizer's two rows in the settings dialog's Workbench card, laid out and drawn offscreen and clicked
// through the input dispatcher.
namespace {
  using luciole::AudioVisualKind;
  using P = luciole::BrowserPreference;

  constexpr int kWidth = 900;
  constexpr int kHeight = 720;

  class Holder final : public Node {
    void doLayout(Renderer& renderer) override {
      for (const auto& child : children()) {
        child->arrange(renderer, {0, 0, width(), height()});
      }
    }
  };

  // Stands in for the dropdown popup, a Wayland surface of its own: it records the options it was asked to show and
  // picks the one the test names.
  class PickingPopup final : public SelectPopupContext {
  public:
    std::vector<std::string> shown;
    std::string pick;

    void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override {
      shown = request.options;
      for (std::size_t index = 0; index < request.options.size(); ++index) {
        if (request.options[index] == pick && callbacks.onSelect) {
          callbacks.onSelect(index);
          return;
        }
      }
      if (callbacks.onDismiss) {
        callbacks.onDismiss();
      }
    }
    void closeSelectDropdown() override {}
    bool isSelectDropdownOpen() const override { return false; }
  };

  template <class T> T* find(Node& node, const std::function<bool(T&)>& match) {
    if (auto* typed = dynamic_cast<T*>(&node); typed != nullptr && match(*typed)) {
      return typed;
    }
    for (const auto& child : node.children()) {
      if (auto* found = find<T>(*child, match)) {
        return found;
      }
    }
    return nullptr;
  }

  Label* labelWithText(Node& root, std::string_view text) {
    return find<Label>(root, [text](Label& label) { return label.text() == text; });
  }

  // The control of the row titled `title`: the first T in the row that holds the title.
  template <class T> T* rowControl(Node& root, std::string_view title) {
    Label* label = labelWithText(root, title);
    TEST_CHECK(label != nullptr);
    for (Node* row = label->parent(); row != nullptr; row = row->parent()) {
      if (auto* control = find<T>(*row, [](T&) { return true; })) {
        return control;
      }
    }
    return nullptr;
  }

  struct Rig {
    ::test::SceneCapture& capture;
    Holder root;
    SettingsDialog* dialog = nullptr;
    PickingPopup popup;
    InputDispatcher dispatcher;
    std::vector<luciole::OverrideEdit> edits;
    std::uint32_t serial = 0;

    explicit Rig(::test::SceneCapture& rig) : capture(rig) {
      dialog = static_cast<SettingsDialog*>(root.addChild(
          std::make_unique<SettingsDialog>(
              [this](luciole::OverrideEdit edit) { edits.push_back(std::move(edit)); }, [] {}, [] {}, [] {}, [] {},
              false
          )
      ));
      dialog->setPopupContext(&popup);
      dispatcher.setSceneRoot(&root);
    }
    ::test::GlPixels frame() { return capture.capture(root, kWidth, kHeight, 1); }
    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      TEST_CHECK(node.width() > 0 && node.height() > 0);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 1000, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
      dispatcher.pointerLeave();
      (void)frame();
    }
    // Opens the Views page and scrolls `node` into sight.
    void reveal(Node& node) {
      Label* views = labelWithText(root, "Views");
      TEST_CHECK(views != nullptr);
      click(*views);
      dialog->reveal(&node);
      for (int i = 0; i < 4; ++i) {
        (void)frame();
      }
    }
  };

  void showsAndChangesBothRows(Rig& rig) {
    luciole::BrowserSettings settings;
    rig.dialog->sync(settings, settings, false, "");
    (void)rig.frame();
    auto* toggle = rowControl<Toggle>(*rig.dialog, "Audio Visualizer");
    auto* select = rowControl<Select>(*rig.dialog, "Visual");
    TEST_CHECK(toggle != nullptr && select != nullptr);
    TEST_CHECK(toggle->checked());
    TEST_CHECK(select->selectedIndex() == 0);
    rig.reveal(*select); // the row below the switch
    const auto pixels = rig.frame();
    test::audio::keepIfAsked(pixels, "settings-workbench-card.png");

    rig.click(*toggle);
    TEST_CHECK(rig.edits.size() == 1);
    TEST_CHECK(rig.edits.back().value(P::WorkbenchAudioVisualizer) == std::optional(false));
    TEST_CHECK(!toggle->checked());

    rig.popup.pick = "Radar";
    rig.click(*select);
    TEST_CHECK(
        (rig.popup.shown
         == std::vector<std::string>{
             "Cosmic Peanut", "Pulsar", "Bars", "Orbit", "Halo", "Radar", "Eclipse", "Dark Sun", "Stargate"
         })
    );
    TEST_CHECK(rig.edits.size() == 2);
    TEST_CHECK(rig.edits.back().value(P::WorkbenchAudioVisual) == std::optional(AudioVisualKind::Radar));
    TEST_CHECK(select->selectedIndex() == 5);
    // Choosing the inherited value again removes the override.
    rig.popup.pick = "Cosmic Peanut";
    rig.click(*select);
    TEST_CHECK(
        rig.edits.size() == 3 && rig.edits.back().preference() == luciole::BrowserPreferenceId::WorkbenchAudioVisual
    );
    TEST_CHECK(!rig.edits.back().value(P::WorkbenchAudioVisual).has_value());

    // A change made elsewhere, such as with the sound preview's buttons, shows in both rows.
    settings.workbench.audioVisualizer = false;
    settings.workbench.audioVisual = AudioVisualKind::DarkSun;
    rig.dialog->sync(settings, luciole::BrowserSettings{}, false, "");
    TEST_CHECK(!toggle->checked());
    TEST_CHECK(select->selectedIndex() == 7);
    test::audio::keepIfAsked(rig.frame(), "settings-workbench-card-dark-sun.png");
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  test::audio::useDarkTheme();
  {
    Rig rig(*capture);
    showsAndChangesBothRows(rig);
  }
  std::println("audio-visual-settings: all checks passed");
  return 0;
}
