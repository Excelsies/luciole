#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "test_check.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_chrome.h"
#include "workbench/pane_layout.h"

#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <variant>

// Header titles show the longest form that fits whole: the title with the reason a pane is closed, the title alone,
// then the pane's name, which alone may be cut. The metrics renderer gives each character half the font size.
namespace {
  using luciole::PaneShell;
  using namespace luciole::workbench;

  const PartLimits kWideRibbon{
      .instrument = {.minWidth = 240, .minHeight = 220},
      .ribbon = {.minWidth = 560, .minHeight = 180, .preferredHeight = 280, .maxHeight = 420},
  };

  std::string tooltipOf(const Label& label) {
    const auto content = label.tooltipContent();
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  float textWidth(std::string_view text) { return static_cast<float>(text.size()) * Style::fontSizeCaption * 0.5F; }

  struct Rig {
    test::MetricsRenderer renderer;
    ::Node root;
    PaneShell* shell = nullptr;

    Rig() {
      root.setSize(4000, 4000);
      shell = static_cast<PaneShell*>(root.addChild(std::make_unique<PaneShell>()));
      shell->setLimits(kWideRibbon);
      shell->setTitle(Pane::Instrument, "Instrument: Nest");
      shell->setTitle(Pane::Ribbon, "Ribbon: Flame graph");
    }

    void layout(float width, float height) { shell->arrange(renderer, {0, 0, width, height}); }
    const Label& title(Pane pane) const { return *shell->header(pane).titleLabel(); }
  };

  // A shown title other than the name fits its label whole.
  void expectWhole(const Label& label) { TEST_CHECK(textWidth(label.text()) <= label.width() + 0.01F); }

  void roomyHeadersShowEverything() {
    Rig rig;
    rig.layout(1400, 700);
    TEST_CHECK(rig.title(Pane::Instrument).text() == "Instrument: Nest");
    TEST_CHECK(rig.title(Pane::Ribbon).text() == "Ribbon: Flame graph");
    TEST_CHECK(rig.title(Pane::Jump).text() == "Jump Ahead");
    TEST_CHECK(!rig.title(Pane::Ribbon).hasTooltip());
    rig.layout(700, 700); // the ribbon closes; its header has room for the reason
    TEST_CHECK(rig.title(Pane::Ribbon).text() == "Ribbon: Flame graph (closed: too narrow)");
    TEST_CHECK(!rig.title(Pane::Ribbon).hasTooltip());
    expectWhole(rig.title(Pane::Ribbon));
  }

  // Without room for the reason the title shows alone and the reason moves to the tooltip.
  void reasonGivesWayFirst() {
    Rig rig;
    const std::string title = "Ribbon: Flame graph of the folders above";
    rig.shell->setTitle(Pane::Ribbon, title);
    rig.layout(700, 700);
    const auto& label = rig.title(Pane::Ribbon);
    TEST_CHECK(textWidth(title + " (closed: too narrow)") > label.width());
    TEST_CHECK(label.text() == title);
    TEST_CHECK(tooltipOf(label) == title + " (closed: too narrow)");
    expectWhole(label);
    rig.layout(700, 700); // an unchanged pass keeps it
    TEST_CHECK(label.text() == title);
    rig.layout(1400, 700); // open again: the whole title, no tooltip
    TEST_CHECK(label.text() == title && !label.hasTooltip());
  }

  // Then the title gives way to the pane's name, and grows back with the room.
  void nameIsTheLastForm() {
    Rig rig;
    const std::string title = "Instrument: Nest of the folders below the current one";
    rig.shell->setTitle(Pane::Instrument, title);
    rig.layout(1400, 700);
    const auto& label = rig.title(Pane::Instrument);
    TEST_CHECK(textWidth(title) > label.width());
    TEST_CHECK(label.text() == "Instrument");
    TEST_CHECK(tooltipOf(label) == title);
    rig.shell->setTitle(Pane::Instrument, "Instrument: Iris");
    rig.layout(1400, 700);
    TEST_CHECK(label.text() == "Instrument: Iris" && !label.hasTooltip());
    expectWhole(label);
  }

  // A header that sets its own text again, as it does when it collapses, gets the fitted form back.
  void headerRefreshKeepsTheFit() {
    Rig rig;
    const std::string title = "Ribbon: Flame graph of the folders above";
    rig.shell->setTitle(Pane::Ribbon, title);
    rig.layout(700, 700);
    auto& header = rig.shell->header(Pane::Ribbon);
    header.setExpanded(true);
    header.setExpanded(false);
    rig.layout(700, 700);
    TEST_CHECK(header.titleLabel()->text() == title);
  }

  // The chrome names the kinds through the shell.
  void chromeTitlesFit() {
    Rig rig;
    luciole::WorkbenchHeaders headers(*rig.shell, [] {});
    headers.show(luciole::WorkbenchInstrument::Atlas, luciole::WorkbenchRibbon::Ladder);
    rig.layout(1400, 700);
    TEST_CHECK(rig.title(Pane::Instrument).text() == "Instrument: Atlas");
    TEST_CHECK(rig.title(Pane::Ribbon).text() == "Ribbon: Ladder");
  }
} // namespace

int main() {
  roomyHeadersShowEverything();
  reasonGivesWayFirst();
  nameIsTheLastForm();
  headerRefreshKeepsTheFit();
  chromeTitlesFit();
  std::println("pane shell title checks passed");
  return 0;
}
