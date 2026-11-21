#include "test_check.h"
#include "ui/workbench/workbench_preferences.h"

#include <cmath>
#include <limits>
#include <print>

namespace {
  using luciole::BrowserPreference;
  using luciole::WorkbenchGeometry;
  using luciole::WorkbenchSettings;

  void defaultsFollowTheView() {
    const auto geometry = luciole::workbenchGeometry(WorkbenchSettings{});
    TEST_CHECK(!geometry.panes.leftWidth);
    TEST_CHECK(!geometry.panes.instrumentHeight);
    for (const auto& height : geometry.panes.ribbonHeights) {
      TEST_CHECK(!height);
    }
    TEST_CHECK(!geometry.panes.collapsed.instrument && !geometry.panes.collapsed.jump);
    TEST_CHECK(!geometry.panes.collapsed.ribbon);
    TEST_CHECK(!geometry.panes.expanded);
    TEST_CHECK(!geometry.panes.columnWidths);
    TEST_NEAR(geometry.columns.columns[0], 260.0, 0.0);
    TEST_NEAR(geometry.columns.columns[1], 190.0, 0.0);
    TEST_NEAR(geometry.columns.columns[2], 140.0, 0.0);
    TEST_CHECK(!geometry.columns.list);
  }

  void savedSizesBecomeGeometry() {
    WorkbenchSettings settings;
    settings.leftWidth = 400;
    settings.instrumentHeight = 300;
    settings.ribbonHeights = {224, 0, 150};
    settings.instrumentCollapsed = true;
    settings.ribbonCollapsed = true;
    settings.columnWidths = {300, 200, 150};
    settings.listWidth = 400;
    const auto geometry = luciole::workbenchGeometry(settings);
    TEST_NEAR(geometry.panes.leftWidth, 400.0, 0.0);
    TEST_NEAR(geometry.panes.instrumentHeight, 300.0, 0.0);
    TEST_NEAR(geometry.panes.ribbonHeights[0], 224.0, 0.0);
    TEST_CHECK(!geometry.panes.ribbonHeights[1]);
    TEST_NEAR(geometry.panes.ribbonHeights[2], 150.0, 0.0);
    TEST_CHECK(geometry.panes.collapsed.instrument);
    TEST_CHECK(!geometry.panes.collapsed.jump);
    TEST_CHECK(geometry.panes.collapsed.ribbon);
    TEST_NEAR(geometry.columns.columns[0], 300.0, 0.0);
    TEST_NEAR(geometry.columns.columns[1], 200.0, 0.0);
    TEST_NEAR(geometry.columns.columns[2], 150.0, 0.0);
    TEST_NEAR(geometry.columns.list, 400.0, 0.0);
  }

  void refusedSizesFollowTheView() {
    WorkbenchSettings settings;
    settings.leftWidth = -5;
    settings.instrumentHeight = 100001;
    settings.ribbonHeights = {1, 100000, -1};
    settings.columnWidths = {139, 641, 640};
    settings.listWidth = 319;
    auto geometry = luciole::workbenchGeometry(settings);
    TEST_CHECK(!geometry.panes.leftWidth);
    TEST_CHECK(!geometry.panes.instrumentHeight);
    TEST_NEAR(geometry.panes.ribbonHeights[0], 1.0, 0.0);
    TEST_NEAR(geometry.panes.ribbonHeights[1], 100000.0, 0.0);
    TEST_CHECK(!geometry.panes.ribbonHeights[2]);
    TEST_NEAR(geometry.columns.columns[0], 260.0, 0.0);
    TEST_NEAR(geometry.columns.columns[1], 190.0, 0.0);
    TEST_NEAR(geometry.columns.columns[2], 640.0, 0.0);
    TEST_CHECK(!geometry.columns.list);
    settings.listWidth = 470;
    TEST_CHECK(!luciole::workbenchGeometry(settings).columns.list);
    settings.listWidth = 469;
    TEST_NEAR(luciole::workbenchGeometry(settings).columns.list, 469.0, 0.0);
    settings.listWidth = 320;
    TEST_NEAR(luciole::workbenchGeometry(settings).columns.list, 320.0, 0.0);
  }

  void geometryBecomesSavedSizes() {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    WorkbenchGeometry geometry;
    geometry.panes.leftWidth = 412.6;
    geometry.panes.instrumentHeight = std::nullopt;
    geometry.panes.ribbonHeights = {nan, 0.2, 2e6};
    geometry.panes.collapsed = {.instrument = false, .jump = true, .ribbon = false};
    geometry.panes.expanded = true;
    geometry.columns.columns = {139.4, nan, 700};
    geometry.columns.list = 300;
    WorkbenchSettings before;
    before.instrument = luciole::WorkbenchInstrument::Atlas;
    before.emphasis = 0.25;
    before.instrumentHeight = 512;
    const auto saved = luciole::withWorkbenchGeometry(before, geometry);
    TEST_CHECK(saved.leftWidth == 413);
    TEST_CHECK(saved.instrumentHeight == 0);
    TEST_CHECK(saved.ribbonHeights[0] == 0);
    TEST_CHECK(saved.ribbonHeights[1] == 1);
    TEST_CHECK(saved.ribbonHeights[2] == 100000);
    TEST_CHECK(!saved.instrumentCollapsed && saved.jumpCollapsed && !saved.ribbonCollapsed);
    TEST_CHECK(saved.columnWidths[0] == 140);
    TEST_CHECK(saved.columnWidths[1] == 190);
    TEST_CHECK(saved.columnWidths[2] == 640);
    TEST_CHECK(saved.listWidth == 320);
    TEST_CHECK(saved.instrument == luciole::WorkbenchInstrument::Atlas);
    TEST_NEAR(saved.emphasis, 0.25, 0.0);

    geometry.columns.list = infinity;
    TEST_CHECK(luciole::withWorkbenchGeometry(before, geometry).listWidth == 0);
    geometry.columns.list = std::nullopt;
    TEST_CHECK(luciole::withWorkbenchGeometry(before, geometry).listWidth == 0);
    geometry.columns.list = 1000;
    TEST_CHECK(luciole::withWorkbenchGeometry(before, geometry).listWidth == 469);
    geometry.panes.leftWidth = -infinity;
    TEST_CHECK(luciole::withWorkbenchGeometry(before, geometry).leftWidth == 0);
  }

  void validSizesRoundTrip() {
    WorkbenchSettings settings;
    settings.leftWidth = 380;
    settings.instrumentHeight = 260;
    settings.ribbonHeights = {200, 300, 0};
    settings.jumpCollapsed = true;
    settings.columnWidths = {140, 640, 333};
    settings.listWidth = 444;
    const auto back = luciole::withWorkbenchGeometry(WorkbenchSettings{}, luciole::workbenchGeometry(settings));
    TEST_CHECK(back.leftWidth == 380);
    TEST_CHECK(back.instrumentHeight == 260);
    TEST_CHECK((back.ribbonHeights == std::array{200, 300, 0}));
    TEST_CHECK(back.jumpCollapsed && !back.instrumentCollapsed && !back.ribbonCollapsed);
    TEST_CHECK((back.columnWidths == std::array{140, 640, 333}));
    TEST_CHECK(back.listWidth == 444);
  }

  void editsNameOnlyChangedFields() {
    const WorkbenchSettings before;
    auto after = before;
    after.leftWidth = 400;
    after.ribbonHeights[1] = 280;
    after.jumpCollapsed = true;
    after.columnWidths[2] = 200;
    after.listWidth = 350;
    after.instrument = luciole::WorkbenchInstrument::Iris;
    const auto edits = luciole::workbenchGeometryEdits(before, after);
    TEST_CHECK(edits.size() == 5);
    TEST_CHECK(edits[0].value(BrowserPreference::WorkbenchLeftWidth) == 400);
    TEST_CHECK(edits[1].value(BrowserPreference::WorkbenchFlowHeight) == 280);
    TEST_CHECK(edits[2].value(BrowserPreference::WorkbenchJumpCollapsed) == true);
    TEST_CHECK(edits[3].value(BrowserPreference::WorkbenchThirdColumnWidth) == 200);
    TEST_CHECK(edits[4].value(BrowserPreference::WorkbenchListWidth) == 350);
    TEST_CHECK(luciole::workbenchGeometryEdits(after, after).empty());

    auto expanded = before;
    expanded.instrumentHeight = 300;
    expanded.ribbonHeights = {180, 0, 127};
    expanded.instrumentCollapsed = true;
    expanded.ribbonCollapsed = true;
    expanded.columnWidths = {200, 150, 141};
    const auto all = luciole::workbenchGeometryEdits(before, expanded);
    TEST_CHECK(all.size() == 8);
    TEST_CHECK(all[0].value(BrowserPreference::WorkbenchInstrumentHeight) == 300);
    TEST_CHECK(all[1].value(BrowserPreference::WorkbenchFlameHeight) == 180);
    TEST_CHECK(all[2].value(BrowserPreference::WorkbenchLadderHeight) == 127);
    TEST_CHECK(all[3].value(BrowserPreference::WorkbenchInstrumentCollapsed) == true);
    TEST_CHECK(all[4].value(BrowserPreference::WorkbenchRibbonCollapsed) == true);
    TEST_CHECK(all[5].value(BrowserPreference::WorkbenchFirstColumnWidth) == 200);
    TEST_CHECK(all[6].value(BrowserPreference::WorkbenchSecondColumnWidth) == 150);
    TEST_CHECK(all[7].value(BrowserPreference::WorkbenchThirdColumnWidth) == 141);
  }

  // The strips pane's width is saved like the other pane sizes: 0 is the default, and 70 to 963 a chosen width: a width
  // past what the pane holds at the system font size is drawn at that most.
  void stripsWidthIsSaved() {
    TEST_CHECK(!luciole::workbenchGeometry(WorkbenchSettings{}).strips);
    WorkbenchSettings settings;
    for (const int width : {70, 200, 321}) {
      settings.stripsWidth = width;
      TEST_NEAR(luciole::workbenchGeometry(settings).strips, static_cast<double>(width), 0.0);
      TEST_CHECK(luciole::withWorkbenchGeometry({}, luciole::workbenchGeometry(settings)).stripsWidth == width);
    }
    for (const int refused : {-1, 69, 322, 100000}) {
      settings.stripsWidth = refused;
      TEST_CHECK(!luciole::workbenchGeometry(settings).strips);
    }
    WorkbenchGeometry geometry;
    TEST_CHECK(luciole::withWorkbenchGeometry({}, geometry).stripsWidth == 0);
    geometry.strips = 240.4;
    TEST_CHECK(luciole::withWorkbenchGeometry({}, geometry).stripsWidth == 240);
    geometry.strips = 20;
    TEST_CHECK(luciole::withWorkbenchGeometry({}, geometry).stripsWidth == 70);
    geometry.strips = 900;
    TEST_CHECK(luciole::withWorkbenchGeometry({}, geometry).stripsWidth == 321);
    geometry.strips = std::numeric_limits<double>::quiet_NaN();
    TEST_CHECK(luciole::withWorkbenchGeometry({}, geometry).stripsWidth == 0);
    WorkbenchSettings before;
    auto after = before;
    after.stripsWidth = 250;
    after.listWidth = 400;
    const auto edits = luciole::workbenchGeometryEdits(before, after);
    TEST_CHECK(edits.size() == 2);
    TEST_CHECK(edits[0].value(BrowserPreference::WorkbenchListWidth) == 400);
    TEST_CHECK(edits[1].value(BrowserPreference::WorkbenchStripsWidth) == 250);
  }

  void choicesFollowSettings() {
    luciole::BrowserSettings settings;
    settings.activationMode = luciole::ActivationMode::SingleClick;
    settings.showHidden = false;
    settings.workbench.instrument = luciole::WorkbenchInstrument::Iris;
    settings.workbench.ribbon = luciole::WorkbenchRibbon::Ladder;
    settings.workbench.strips = false;
    settings.workbench.sizeBy = luciole::WorkbenchMeasure::Files;
    settings.workbench.colorBy = luciole::WorkbenchColor::Modified;
    settings.workbench.emphasis = 0.75;
    const auto choices = luciole::workbenchChoices(settings, true);
    TEST_CHECK(choices.instrument == luciole::WorkbenchInstrument::Iris);
    TEST_CHECK(choices.ribbon == luciole::WorkbenchRibbon::Ladder);
    TEST_CHECK(!choices.strips);
    TEST_CHECK(choices.layout.sizeBy == luciole::WorkbenchMeasure::Files);
    TEST_CHECK(choices.layout.colorBy == luciole::WorkbenchColor::Modified);
    TEST_NEAR(choices.layout.emphasis, 0.75, 0.0);
    TEST_CHECK(choices.layout.showHidden);
    TEST_CHECK(choices.activation == luciole::ActivationMode::SingleClick);
    TEST_CHECK(!luciole::workbenchChoices(settings, false).layout.showHidden);
  }
  // A saved step is the preview's size; anything else reads as 100 percent.
  void previewSizeIsAStep() {
    WorkbenchSettings settings;
    TEST_CHECK(luciole::workbenchPreviewSize(settings) == 100);
    for (const int step : {70, 125, 300}) {
      settings.previewSize = step;
      TEST_CHECK(luciole::workbenchPreviewSize(settings) == step);
    }
    for (const int bad : {0, 69, 75, 301, -1, std::numeric_limits<int>::min()}) {
      settings.previewSize = bad;
      TEST_CHECK(luciole::workbenchPreviewSize(settings) == 100);
    }
  }
} // namespace

int main() {
  defaultsFollowTheView();
  savedSizesBecomeGeometry();
  refusedSizesFollowTheView();
  geometryBecomesSavedSizes();
  validSizesRoundTrip();
  editsNameOnlyChangedFields();
  stripsWidthIsSaved();
  choicesFollowSettings();
  previewSizeIsAStep();
  std::println("workbench preferences checks passed");
}
