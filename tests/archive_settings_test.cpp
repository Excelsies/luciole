#include "audio_visual_clips.h"
#include "config/settings.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/select_popup_context.h"
#include "ui/controls/toggle.h"
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

// The Archive-Like Files page of the settings dialog: one switch per archive container type, laid out and drawn
// offscreen and clicked through the input dispatcher.
namespace {
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
    std::optional<std::uint32_t> serial; // the grab serial the dropdown was asked to use

    void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override {
      shown = request.options;
      serial = request.inputSerial;
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

  Toggle* rowToggle(Node& root, std::string_view title) {
    Label* label = labelWithText(root, title);
    TEST_CHECK(label != nullptr);
    for (Node* row = label->parent(); row != nullptr; row = row->parent()) {
      if (auto* toggle = find<Toggle>(*row, [](Toggle&) { return true; })) {
        return toggle;
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
    std::uint32_t pressSerial = 0; // of the last click

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
      pressSerial = ++serial;
      dispatcher.pointerButton(x, y, BTN_LEFT, true, pressSerial, 1000, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 1100, false);
      dispatcher.pointerLeave();
      (void)frame();
    }
    void reveal(Node& node) {
      Label* page = labelWithText(root, "Archive-Like Files");
      TEST_CHECK(page != nullptr);
      click(*page);
      dialog->reveal(&node);
      for (int i = 0; i < 4; ++i) {
        (void)frame();
      }
    }
  };

  luciole::ArchiveContainerSet setOf(std::initializer_list<std::string_view> names) {
    luciole::ArchiveContainerSet set;
    for (const auto name : names) {
      set.set(*luciole::archiveContainerByName(name));
    }
    return set;
  }

  void everyTypeHasASwitchUnderItsFamily(Rig& rig) {
    luciole::BrowserSettings settings;
    rig.dialog->sync(settings, settings, false, "");
    (void)rig.frame();
    for (std::size_t family = 0; family < luciole::kArchiveFamilyCount; ++family) {
      TEST_CHECK(
          labelWithText(*rig.dialog, luciole::archiveFamilyLabel(static_cast<luciole::ArchiveFamily>(family)))
          != nullptr
      );
    }
    // The caption describes every card, so it sits above the first card's heading.
    auto* caption = labelWithText(
        *rig.dialog, "A type that is switched on opens in the archive browser instead of its default application."
    );
    auto* first = labelWithText(*rig.dialog, luciole::archiveFamilyLabel(static_cast<luciole::ArchiveFamily>(0)));
    TEST_CHECK(caption != nullptr && first != nullptr);
    if (caption != nullptr && first != nullptr) {
      rig.reveal(*caption);
      float captionX = 0;
      float captionY = 0;
      float firstX = 0;
      float firstY = 0;
      Node::absolutePosition(caption, captionX, captionY);
      Node::absolutePosition(first, firstX, firstY);
      TEST_CHECK(captionY + caption->height() <= firstY);
    }
    for (const auto& type : luciole::kArchiveContainerTypes) {
      auto* toggle = rowToggle(*rig.dialog, type.label);
      TEST_CHECK(toggle != nullptr && !toggle->checked());
    }
  }

  void aSwitchSavesTheWholeList(Rig& rig) {
    luciole::BrowserSettings settings;
    rig.dialog->sync(settings, settings, false, "");
    auto* jar = rowToggle(*rig.dialog, "Java Archive (.jar)");
    auto* epub = rowToggle(*rig.dialog, "EPUB Book (.epub)");
    TEST_CHECK(jar != nullptr && epub != nullptr);
    rig.reveal(*jar);
    test::audio::keepIfAsked(rig.frame(), "settings-archive-like-files.png");
    rig.click(*jar);
    TEST_CHECK(rig.edits.size() == 1);
    TEST_CHECK(rig.edits.back().value(P::OpenAsArchive) == std::optional(setOf({"jar"})));
    TEST_CHECK(jar->checked() && !epub->checked());
    rig.reveal(*epub);
    rig.click(*epub);
    TEST_CHECK(rig.edits.back().value(P::OpenAsArchive) == std::optional(setOf({"jar", "epub"})));
    // Switching the last one off restores the inherited value, which removes the override.
    rig.reveal(*jar);
    rig.click(*jar);
    rig.reveal(*epub);
    rig.click(*epub);
    TEST_CHECK(
        rig.edits.back().preference() == luciole::BrowserPreferenceId::OpenAsArchive
        && !rig.edits.back().value(P::OpenAsArchive).has_value()
    );
    TEST_CHECK(!jar->checked() && !epub->checked());
    // A change made elsewhere shows in the switches.
    settings.openAsArchive = setOf({"apk"});
    rig.dialog->sync(settings, luciole::BrowserSettings{}, false, "");
    TEST_CHECK(rowToggle(*rig.dialog, "Android Package (.apk)")->checked() && !jar->checked());
  }

  // With a list in the base file, switching every type off is an override of [], not a removed override.
  void switchingEveryTypeOffOverridesANonEmptyBase(Rig& rig) {
    luciole::BrowserSettings base;
    base.openAsArchive = setOf({"jar", "epub"});
    rig.dialog->sync(base, base, false, "");
    auto* jar = rowToggle(*rig.dialog, "Java Archive (.jar)");
    auto* epub = rowToggle(*rig.dialog, "EPUB Book (.epub)");
    TEST_CHECK(jar != nullptr && epub != nullptr && jar->checked() && epub->checked());
    rig.edits.clear();
    rig.reveal(*jar);
    rig.click(*jar);
    rig.reveal(*epub);
    rig.click(*epub);
    TEST_CHECK(rig.edits.size() == 2);
    TEST_CHECK(rig.edits.back().preference() == luciole::BrowserPreferenceId::OpenAsArchive);
    TEST_CHECK(rig.edits.back().value(P::OpenAsArchive) == std::optional(luciole::ArchiveContainerSet{}));
    TEST_CHECK(!jar->checked() && !epub->checked());
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
    everyTypeHasASwitchUnderItsFamily(rig);
    aSwitchSavesTheWholeList(rig);
    switchingEveryTypeOffOverridesANonEmptyBase(rig);
  }
  std::println("archive-settings: all checks passed");
  return 0;
}
