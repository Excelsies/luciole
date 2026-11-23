#include "config/settings.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "system_text_scale.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/select_popup_context.h"
#include "ui/dialogs/settings_dialog.h"
#include "ui/workbench/ancestor_strips.h"
#include "ui/workbench/pane_shell.h"
#include "workbench/column_plan.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// The strips, the pane headers and the Settings button, laid out with the toolkit's text shaping at the text scales
// of a 14 pt, 11 pt, 16 pt and 21 pt system font: a strip's name keeps room to either side, a header holds its
// title and the button shows its whole label. At scale 1 the sizes are the ones the plan and the layout give.
namespace {
  using namespace luciole;
  namespace wb = luciole::workbench;
  using ::test::SceneCapture;

  constexpr float kLarge = 16.0F * 96.0F / 72.0F / 14.0F;
  constexpr std::array<float, 4> kScales{1.0F, 44.0F / 42.0F, kLarge, 2.0F};

  class Holder final : public Node {
    void doLayout(Renderer& renderer) override {
      for (const auto& child : children()) {
        child->arrange(renderer, {0, 0, width(), height()});
      }
    }
  };

  class NoPopup final : public SelectPopupContext {
    void openSelectDropdown(const DropdownRequest&, DropdownCallbacks) override {}
    void closeSelectDropdown() override {}
    bool isSelectDropdownOpen() const override { return false; }
  };

  Label* labelWithText(Node& node, std::string_view text) {
    if (auto* label = dynamic_cast<Label*>(&node); label != nullptr && label->text() == text) {
      return label;
    }
    for (const auto& child : node.children()) {
      if (auto* found = labelWithText(*child, text)) {
        return found;
      }
    }
    return nullptr;
  }

  void stripsKeepRoom(SceneCapture& capture, float scale) {
    Holder root;
    auto* strips = static_cast<AncestorStrips*>(root.addChild(std::make_unique<AncestorStrips>()));
    std::vector<AncestorEntry> ancestors;
    ancestors.reserve(5);
    for (std::uint64_t i = 0; i < 5; ++i) {
      ancestors.push_back({.name = std::format("folder number {}", i), .path = "/x", .id = i});
    }
    strips->setAncestors(ancestors);
    strips->setTextScale(scale);
    wb::PlanOptions options;
    options.textScale = scale;
    const auto plan = wb::planColumns(1100, 5, options);
    strips->setPlan(plan.strips);
    root.setSize(static_cast<float>(plan.stripsWidth), 400);
    (void)capture.capture(root, static_cast<int>(plan.stripsWidth), 400, 1.0F);
    std::size_t named = 0;
    for (std::size_t i = 0; i < strips->count(); ++i) {
      const auto* label = strips->label(i);
      if (label == nullptr) {
        continue;
      }
      ++named;
      const auto strip = static_cast<float>(plan.strips[i].width);
      // the label is laid flat and turned: its glyphs fill an em across the strip
      const float side = (strip - label->fontSize() * scale) / 2;
      std::println("strip {} {:.0f} px wide at {:.2f}: {:.1f} px to either side", i, strip, scale, side);
      TEST_CHECK(side >= 2.0F);
    }
    TEST_CHECK(named >= 3);
    if (scale == 1.0F) {
      TEST_CHECK(plan.stripsWidth == wb::kStripsDefaultWidth);
      TEST_CHECK(plan.strips.back().width == 44);
    } else {
      TEST_CHECK(plan.strips.back().width == std::round(44 * scale));
    }
  }

  void headerHoldsItsTitle(SceneCapture& capture, float scale) {
    PaneShell shell;
    shell.setSize(1600, 1200);
    (void)capture.capture(shell, 1600, 1200, 1.0F);
    auto& header = shell.header(wb::Pane::Instrument);
    const float line = header.titleLabel()->height();
    std::println("header {:.1f} px for a {:.1f} px line at {:.2f}", header.height(), line, scale);
    TEST_CHECK(header.height() >= line + 2);
    if (scale == 1.0F) {
      TEST_CHECK(header.height() == PaneHeader::kHeight);
    }
  }

  void clearVisitsShowsWhole(SceneCapture& capture, float scale) {
    Holder root;
    NoPopup popup;
    auto* dialog = static_cast<SettingsDialog*>(
        root.addChild(std::make_unique<SettingsDialog>([](const OverrideEdit&) {}, [] {}, [] {}, [] {}, [] {}, false))
    );
    dialog->setPopupContext(&popup);
    root.setSize(900, 720);
    BrowserSettings settings;
    dialog->sync(settings, settings, false, "");
    (void)capture.capture(root, 900, 720, 1.0F);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&root);
    auto* views = labelWithText(root, "Views");
    TEST_CHECK(views != nullptr);
    float x = 0;
    float y = 0;
    Node::absolutePosition(views, x, y);
    x += views->width() / 2 - 2;
    y += views->height() / 2 - 2;
    dispatcher.pointerEnter(x + 2, y + 2, 1);
    dispatcher.pointerButton(x + 2, y + 2, BTN_LEFT, true, 2, 1000, false);
    dispatcher.pointerButton(x + 2, y + 2, BTN_LEFT, false, 3, 1100, false);
    for (int i = 0; i < 3; ++i) {
      (void)capture.capture(root, 900, 720, 1.0F);
    }
    auto* label = labelWithText(root, "Clear Visit History");
    TEST_CHECK(label != nullptr);
    const float whole = capture.textWidth(label->text(), label->fontSize());
    std::println("Clear Visit History: label {:.1f} px for {:.1f} px of text at {:.2f}", label->width(), whole, scale);
    TEST_CHECK(label->width() + 0.5F >= whole);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  for (const float wanted : kScales) {
    const float scale = ::test::setSystemTextScale(*capture, wanted);
    stripsKeepRoom(*capture, scale);
    headerHoldsItsTitle(*capture, scale);
    clearVisitsShowsWhole(*capture, scale);
  }
  std::println("workbench-text-scale-chrome: all checks passed");
  return 0;
}
