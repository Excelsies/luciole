#include "audio_visual_clips.h"
#include "config/settings.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/select_popup_context.h"
#include "ui/dialogs/settings_dialog.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// The row for a preview with no column to show in, in the settings dialog's Workbench card, laid out and drawn
// offscreen and clicked through the input dispatcher.
namespace {
  using luciole::WorkbenchPreviewPlace;
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

  Select* rowSelect(Node& root, std::string_view title) {
    Label* label = labelWithText(root, title);
    TEST_CHECK(label != nullptr);
    for (Node* row = label->parent(); row != nullptr; row = row->parent()) {
      if (auto* select = find<Select>(*row, [](Select&) { return true; })) {
        return select;
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

  void showsAndChangesTheRow(Rig& rig) {
    luciole::BrowserSettings settings;
    rig.dialog->sync(settings, settings, false, "");
    (void)rig.frame();
    auto* select = rowSelect(*rig.dialog, "Preview Placement");
    TEST_CHECK(select != nullptr);
    TEST_CHECK(select->selectedIndex() == 0);
    rig.reveal(*select);
    test::audio::keepIfAsked(rig.frame(), "settings-preview-place-row.png");

    rig.popup.pick = "Hide When It Does Not Fit";
    rig.click(*select);
    TEST_CHECK(
        (rig.popup.shown
         == std::vector<std::string>{
             "Replace Jump Ahead When It Does Not Fit", "Always Replace Jump Ahead", "Hide When It Does Not Fit"
         })
    );
    TEST_CHECK(rig.edits.size() == 1);
    TEST_CHECK(
        rig.edits.back().value(P::WorkbenchPreviewPlace) == std::optional(WorkbenchPreviewPlace::HiddenWhenNarrow)
    );
    TEST_CHECK(select->selectedIndex() == 2);
    rig.popup.pick = "Always Replace Jump Ahead";
    rig.click(*select);
    TEST_CHECK(
        rig.edits.back().value(P::WorkbenchPreviewPlace) == std::optional(WorkbenchPreviewPlace::JumpAheadAlways)
    );
    TEST_CHECK(select->selectedIndex() == 1);
    // Choosing the inherited value again removes the override.
    rig.popup.pick = "Replace Jump Ahead When It Does Not Fit";
    rig.click(*select);
    TEST_CHECK(
        rig.edits.size() == 3
        && rig.edits.back().preference() == luciole::BrowserPreferenceId::WorkbenchPreviewPlace
        && !rig.edits.back().value(P::WorkbenchPreviewPlace).has_value()
    );
    // A change made elsewhere shows in the row.
    settings.workbench.previewPlace = WorkbenchPreviewPlace::HiddenWhenNarrow;
    rig.dialog->sync(settings, luciole::BrowserSettings{}, false, "");
    TEST_CHECK(select->selectedIndex() == 2);
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
    showsAndChangesTheRow(rig);
  }
  std::println("preview-place-settings: all checks passed");
  return 0;
}
