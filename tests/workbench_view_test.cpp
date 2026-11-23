#include "core/input/key_modifiers.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/workbench/ancestor_strips.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench_view_rig.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <print>
#include <string>

namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;
  using std::chrono::milliseconds;

  std::vector<const Button*> segments(const Segmented& choice) { return visible<Button>(choice); }

  std::string title(const Rig& rig, wb::Pane pane) { return rig.view->shell().header(pane).titleLabel()->text(); }

  void everyPaneShowsTheTree() {
    Rig rig;
    rig.settle();
    const auto snapshot = rig.model.snapshot();
    const auto& tree = snapshot->tree;
    // the instrument draws every entry, the flame graph every folder
    std::size_t entries = 0;
    std::size_t folders = 0;
    for (const auto id : wb::children(tree, tree.root)) {
      ++entries;
      TEST_CHECK(rig.piece(rig.view->instrument(), id).has_value());
      if (tree.nodes[id].folder) {
        ++folders;
        TEST_CHECK(rig.piece(rig.view->ribbon(), id).has_value());
      }
    }
    TEST_CHECK(entries == 4 && folders == 3);
    // and the levels below: big/deep and big/deep/inner in the strata ahead
    TEST_CHECK(rig.piece(rig.view->ribbon(), rig.node("big/deep")).has_value());
    TEST_CHECK(rig.piece(rig.view->ribbon(), rig.node("big/deep/inner")).has_value());
    TEST_CHECK(visible<JumpRowTile>(rig.view->jumpList()).size() == 3);
    for (const char* path : {"big/deep", "big/deep/inner", "mid/sub"}) {
      TEST_CHECK(rig.jumpRow(path) != nullptr);
    }
    // with no cursor the columns follow the heaviest folder at each level
    TEST_CHECK(rig.lookahead.location(0) == rig.fixture.at("big"));
    TEST_CHECK(rig.lookahead.location(1) == rig.fixture.at("big/deep"));
    TEST_CHECK(visible<QuietRow>(rig.view->column(0)).size() == 2);
    const auto* deep = rig.columnRow(0, "deep");
    TEST_CHECK(deep != nullptr && deep->share().has_value() && deep->marks().followed);
    TEST_CHECK(rig.columnRow(0, "b.bin") != nullptr);
    TEST_CHECK(rig.view->center().strips().count() >= 1);
    TEST_CHECK(title(rig, wb::Pane::Instrument) == "Instrument: Nest");
    TEST_CHECK(title(rig, wb::Pane::Ribbon) == "Ribbon: Flame Graph");
    TEST_CHECK(title(rig, wb::Pane::Jump) == "Jump Ahead");
    TEST_CHECK(rig.gone.empty() && rig.geometries.empty() && rig.instruments.empty());
  }

  void hoverLinksEveryPane() {
    Rig rig;
    rig.settle();
    const auto& instrument = rig.view->instrument().shapeView();
    const auto& ribbon = rig.view->ribbon().shapeView();
    const auto big = rig.node("big");
    rig.move(*rig.piece(rig.view->instrument(), big));
    rig.frame();
    TEST_CHECK(rig.state.hovered() && rig.state.hovered()->pane == WorkbenchPane::Instrument);
    TEST_CHECK(rig.state.hovered()->entry.location == rig.fixture.at("big"));
    TEST_CHECK(ribbon.marks().linked == big && instrument.marks().linked == wb::kNoNode);
    TEST_CHECK(!rig.listMarks.empty() && rig.listMarks.back() == rig.fixture.at("big"));

    // a row of column A lights the others, Jump Ahead included, and the list row of its first-level folder
    const auto deep = rig.node("big/deep");
    rig.move(middle(*rig.columnRow(0, "deep")));
    rig.frame();
    TEST_CHECK(rig.state.hovered() && rig.state.hovered()->pane == WorkbenchPane::ColumnA);
    TEST_CHECK(instrument.marks().linked == deep && ribbon.marks().linked == deep);
    TEST_CHECK(rig.jumpRow("big/deep")->marks().linked && !rig.jumpRow("mid/sub")->marks().linked);
    TEST_CHECK(rig.listMarks.back() == rig.fixture.at("big"));

    // a Jump Ahead row lights the parts and the list
    const auto sub = rig.node("mid/sub");
    rig.move(middle(*rig.jumpRow("mid/sub")));
    rig.frame();
    TEST_CHECK(rig.state.hovered() && rig.state.hovered()->pane == WorkbenchPane::JumpAhead);
    TEST_CHECK(instrument.marks().linked == sub && ribbon.marks().linked == sub);
    TEST_CHECK(!rig.jumpRow("mid/sub")->marks().linked);
    TEST_CHECK(rig.listMarks.back() == rig.fixture.at("mid"));

    // the ribbon lights the instrument, and a row of the hovered folder in a column
    rig.move(*rig.piece(rig.view->ribbon(), deep));
    rig.frame();
    TEST_CHECK(rig.state.hovered() && rig.state.hovered()->pane == WorkbenchPane::Ribbon);
    TEST_CHECK(instrument.marks().linked == deep && ribbon.marks().linked == wb::kNoNode);
    TEST_CHECK(rig.columnRow(0, "deep")->marks().linked);

    // the list's own hover comes from the page through the state
    rig.state.hover(WorkbenchPane::List, {.location = rig.fixture.at("mid"), .folder = true});
    rig.frame();
    TEST_CHECK(instrument.marks().linked == rig.node("mid") && ribbon.marks().linked == rig.node("mid"));
    TEST_CHECK(!rig.listMarks.back().has_value());
    rig.state.unhover(WorkbenchPane::List);
    rig.frame();
    TEST_CHECK(instrument.marks().linked == wb::kNoNode && !rig.columnRow(0, "deep")->marks().linked);
  }

  void clicksGoWhereTheyPoint() {
    Rig rig;
    auto choices = rig.view->choices();
    choices.activation = ActivationMode::SingleClick;
    rig.view->setChoices(choices);
    rig.settle();
    rig.click(*rig.piece(rig.view->instrument(), rig.node("mid")));
    TEST_CHECK(rig.gone.size() == 1 && rig.gone.back() == rig.fixture.at("mid"));
    rig.click(*rig.piece(rig.view->ribbon(), rig.node("small")));
    TEST_CHECK(rig.gone.size() == 2 && rig.gone.back() == rig.fixture.at("small"));
    rig.click(*rig.jumpRow("mid/sub"));
    TEST_CHECK(rig.gone.size() == 3 && rig.gone.back() == rig.fixture.at("mid/sub"));
    rig.click(*rig.columnRow(0, "deep"));
    TEST_CHECK(rig.gone.size() == 4 && rig.gone.back() == rig.fixture.at("big/deep"));
    auto& strips = rig.view->center().strips();
    rig.click(*strips.area(strips.count() - 1)); // the nearest strip, beside the list
    TEST_CHECK(rig.gone.size() == 5 && rig.gone.back() == Location::fromPath(rig.fixture.base.native()));
    rig.frame();
    TEST_CHECK(!rig.focuses.empty() && rig.focuses.back() == rig.list);

    // files: a piece selects, a column row opens, a right press asks for a menu
    rig.click(*rig.piece(rig.view->instrument(), rig.node("note.txt")));
    TEST_CHECK(rig.selected.size() == 1 && rig.selected.back() == rig.fixture.at("note.txt"));
    rig.click(*rig.columnRow(0, "b.bin"));
    TEST_CHECK(rig.opened.size() == 1 && rig.opened.back() == rig.fixture.at("big/b.bin"));
    rig.click(*rig.piece(rig.view->instrument(), rig.node("mid")), BTN_RIGHT);
    TEST_CHECK(rig.menus.size() == 1 && rig.menus.back().pane == WorkbenchPane::Instrument);
    TEST_CHECK(rig.menus.back().entry == rig.fixture.at("mid"));
    const auto at = middle(*rig.columnRow(0, "deep"));
    rig.click(at, BTN_RIGHT);
    TEST_CHECK(rig.menus.size() == 2 && rig.menus.back().pane == WorkbenchPane::ColumnA);
    TEST_CHECK(rig.menus.back().entry == rig.fixture.at("big/deep") && rig.menus.back().sceneX == at.x);
    TEST_CHECK(rig.gone.size() == 5);
  }

  // The owner learns the button and the modifiers of a click, so it can open a middle click in a new tab.
  void clicksCarryButtonAndModifiers() {
    Rig rig;
    rig.settle();
    rig.held = KeyMod::Shift;
    rig.click(*rig.piece(rig.view->instrument(), rig.node("mid")), BTN_MIDDLE);
    TEST_CHECK(rig.requests.size() == 1 && rig.gone.back() == rig.fixture.at("mid"));
    TEST_CHECK(rig.requests.back().button == BTN_MIDDLE && rig.requests.back().modifiers == KeyMod::Shift);
    rig.held = 0;
    rig.click(*rig.piece(rig.view->ribbon(), rig.node("small")));
    TEST_CHECK(rig.requests.size() == 2 && rig.requests.back().button == BTN_LEFT);
    TEST_CHECK(rig.requests.back().modifiers == 0);
    rig.held = KeyMod::Ctrl;
    rig.click(*rig.columnRow(0, "deep"));
    rig.click(*rig.columnRow(0, "deep"));
    TEST_CHECK(rig.requests.size() == 3 && rig.gone.back() == rig.fixture.at("big/deep"));
    TEST_CHECK(rig.requests.back().button == BTN_LEFT && rig.requests.back().modifiers == KeyMod::Ctrl);
  }

  void doubleClickPinsThenOpens() {
    Rig rig;
    rig.settle();
    rig.click(*rig.columnRow(0, "deep"));
    rig.frame();
    TEST_CHECK(rig.gone.empty() && rig.state.locked());
    TEST_CHECK((rig.state.pinned(0) == WorkbenchEntry{.location = rig.fixture.at("big/deep"), .folder = true}));
    TEST_CHECK(rig.columnRow(0, "deep")->marks().pinned && rig.view->column(0).notice() == QuietNotice::None);
    rig.click(*rig.columnRow(0, "deep"));
    TEST_CHECK(rig.gone.size() == 1 && rig.gone.back() == rig.fixture.at("big/deep"));
  }

  void headersSwitchKinds() {
    Rig rig;
    rig.settle();
    const auto instruments = segments(rig.view->instrumentSwitch());
    const auto ribbons = segments(rig.view->ribbonSwitch());
    TEST_CHECK(instruments.size() == 3 && ribbons.size() == 3);
    rig.click(*instruments[1]);
    TEST_CHECK((rig.instruments == std::vector{WorkbenchInstrument::Iris}));
    rig.frame();
    TEST_CHECK(rig.view->instrument().kind() == WorkbenchInstrument::Iris);
    TEST_CHECK(title(rig, wb::Pane::Instrument) == "Instrument: Iris");
    rig.click(*ribbons[2]);
    TEST_CHECK((rig.ribbons == std::vector{WorkbenchRibbon::Ladder}));
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.view->ribbon().kind() == WorkbenchRibbon::Ladder);
    TEST_CHECK(title(rig, wb::Pane::Ribbon) == "Ribbon: Ladder");
    // the ribbon pane takes the ladder's preferred height
    const auto* body = rig.view->shell().content(PaneRegion::Ribbon);
    TEST_NEAR(body->height(), rig.view->ribbon().limits().preferredHeight, 0.5);
    // the page confirming the choice reports nothing; the page changing it moves the switch without a report
    auto choices = rig.view->choices();
    rig.view->setChoices(choices);
    choices.instrument = WorkbenchInstrument::Atlas;
    rig.view->setChoices(choices);
    rig.frame();
    TEST_CHECK(rig.instruments.size() == 1 && rig.ribbons.size() == 1);
    TEST_CHECK(rig.view->instrumentSwitch().selectedIndex() == 2);
    TEST_CHECK(rig.view->instrument().kind() == WorkbenchInstrument::Atlas);
    TEST_CHECK(title(rig, wb::Pane::Instrument) == "Instrument: Atlas");
  }

  void panesCollapseExpandAndRestore() {
    Rig rig;
    rig.settle();
    auto& shell = rig.view->shell();
    const std::array<std::pair<wb::Pane, const Node*>, 3> panes{{
        {wb::Pane::Instrument, &rig.view->instrument()},
        {wb::Pane::Jump, &rig.view->jumpList()},
        {wb::Pane::Ribbon, &rig.view->ribbon()},
    }};
    for (const auto& [pane, content] : panes) {
      rig.click(*shell.header(pane).collapseButton());
      rig.frame();
      const auto collapsed = rig.geometries.back().panes.collapsed;
      TEST_CHECK(
          pane == wb::Pane::Instrument ? collapsed.instrument
              : pane == wb::Pane::Jump ? collapsed.jump
                                       : collapsed.ribbon
      );
      TEST_CHECK(!shown(content));
      rig.click(*shell.header(pane).collapseButton());
      rig.frame();
      TEST_CHECK(rig.geometries.back().panes.collapsed == wb::PaneFlags{});
      TEST_CHECK(shown(content));
    }
    TEST_CHECK(rig.geometries.size() == 6 && rig.focuses.empty());
    TEST_CHECK(!shown(&rig.view->resetButton(wb::Pane::Instrument)) && shown(&rig.view->resetButton(wb::Pane::Ribbon)));

    rig.click(*shell.header(wb::Pane::Instrument).expandButton());
    rig.frame();
    TEST_CHECK(rig.geometries.back().panes.expanded && rig.state.expanded());
    TEST_CHECK(rig.focuses.size() == 1 && rig.focuses.back() == &rig.view->instrument().shapeView().inputArea());
    TEST_CHECK(!shown(&rig.view->jumpList()) && !shown(&rig.view->ribbon()) && !shown(&rig.view->center()));
    TEST_NEAR(rig.view->instrument().width(), rig.width, 0.5);
    TEST_CHECK(shown(&rig.view->resetButton(wb::Pane::Instrument)));

    // navigation while expanded shows in the other panes once the instrument returns
    rig.open("mid");
    rig.until([&] {
      const auto snapshot = rig.model.snapshot();
      return snapshot
          && snapshot->location == rig.fixture.at("mid")
          && !rig.model.measuring()
          && rig.view->ribbon().shownSnapshot().serial == snapshot->serial;
    });
    rig.click(*shell.header(wb::Pane::Instrument).expandButton());
    rig.frame();
    TEST_CHECK(!rig.geometries.back().panes.expanded && !rig.state.expanded());
    TEST_CHECK(rig.focuses.size() == 2 && rig.focuses.back() == rig.list);
    TEST_CHECK(shown(&rig.view->ribbon()) && shown(&rig.view->center()));
    TEST_CHECK(rig.piece(rig.view->ribbon(), rig.node("mid/sub")).has_value());
    TEST_CHECK(!shown(&rig.view->resetButton(wb::Pane::Instrument)));

    // Reset layout restores the defaults and reports them
    rig.click(*shell.header(wb::Pane::Jump).collapseButton());
    rig.click(rig.view->resetButton(wb::Pane::Ribbon));
    rig.frame();
    TEST_CHECK(rig.geometries.back() == WorkbenchGeometry{});
    TEST_CHECK(shown(&rig.view->jumpList()));
  }

  void columnsFollowCursorRestAndPin() {
    Rig rig;
    rig.settle();
    const auto lists = [&rig](const char* folder) {
      rig.until([&] {
        return rig.lookahead.count() > 0
            && rig.lookahead.location(0) == rig.fixture.at(folder)
            && rig.lookahead.model(0).snapshot().status == DirectoryStatus::Ready;
      });
    };
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("mid"), .folder = true});
    lists("mid");
    // a pointer resting on a list row takes over after the dwell
    rig.state.pointerInList(WorkbenchEntry{.location = rig.fixture.at("small"), .folder = true}, 10, 10, rig.now);
    rig.now += milliseconds(200);
    rig.frame();
    TEST_CHECK(rig.lookahead.location(0) == rig.fixture.at("mid"));
    rig.now += milliseconds(100);
    lists("small");

    // a click pin holds against a resting pointer until the cursor moves
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true});
    lists("big");
    rig.click(*rig.columnRow(0, "deep"));
    rig.frame();
    TEST_CHECK(rig.lookahead.location(1) == rig.fixture.at("big/deep"));
    rig.state.pointerInList(WorkbenchEntry{.location = rig.fixture.at("mid"), .folder = true}, 50, 50, rig.now);
    rig.now += milliseconds(400);
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.lookahead.location(0) == rig.fixture.at("big") && rig.state.locked());
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("small"), .folder = true});
    lists("small");
    TEST_CHECK(!rig.state.locked());
  }

  void columnRowsRestAndPin() {
    Rig rig;
    rig.settle();
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true});
    rig.until([&] { return rig.columnRow(0, "b.bin") != nullptr; });
    const LookaheadPreview file{.file = rig.fixture.at("big/b.bin"), .slot = 1};
    // a pointer resting on a row of column A steers column B until it leaves the columns
    rig.move(middle(*rig.columnRow(0, "b.bin")));
    rig.frame();
    TEST_CHECK(rig.lookahead.location(1) == rig.fixture.at("big/deep") && !rig.state.pinned(0));
    rig.now += WorkbenchState::kDwell;
    rig.until([&] { return rig.lookahead.preview() == file; });
    TEST_CHECK(rig.state.pinned(0).has_value() && !rig.state.locked());
    rig.move(middle(*rig.list));
    rig.now += WorkbenchState::kRelease;
    rig.until([&] { return rig.lookahead.count() > 1 && rig.lookahead.location(1) == rig.fixture.at("big/deep"); });
    TEST_CHECK(!rig.state.pinned(0) && !rig.lookahead.preview());

    // the header's pin control pins what column B lists, then unpins it
    const auto pin = visible<Button>(rig.view->column(0));
    TEST_CHECK(pin.size() == 1);
    rig.click(*pin[0]);
    rig.frame();
    TEST_CHECK(rig.state.locked());
    TEST_CHECK((rig.state.pinned(0) == WorkbenchEntry{.location = rig.fixture.at("big/deep"), .folder = true}));
    rig.click(*pin[0]);
    rig.frame();
    TEST_CHECK(!rig.state.locked() && !rig.state.pinned(0));
  }

  void newStateLearnsTheCursor() {
    Rig rig;
    const WorkbenchEntry mid{.location = rig.fixture.at("mid"), .folder = true};
    rig.view->setCursor(mid);
    WorkbenchState other;
    rig.view->setState(&other);
    rig.view->setCursor(mid);
    TEST_CHECK(other.target() == mid);
    rig.view->setState(nullptr);
  }

  // Hover and the cursor reach each part in the nodes of the snapshot it shows, while Atlas lays out a newer one.
  void partsMarkTheSnapshotTheyShow() {
    Rig rig;
    auto choices = rig.view->choices();
    choices.instrument = WorkbenchInstrument::Atlas;
    rig.view->setChoices(choices);
    rig.settle();
    const auto before = placeSnapshot(rig.model.snapshot());
    std::ofstream(rig.fixture.root / "fresh.bin") << std::string(5000, 'x');
    rig.model.invalidateChild("fresh.bin");
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (rig.model.measuring() || rig.model.snapshot()->node("fresh.bin") == wb::kNoNode) {
      TEST_CHECK(Clock::now() < deadline);
      rig.pump.once();
    }
    const WorkbenchEntry fresh{.location = rig.fixture.at("fresh.bin"), .folder = false};
    const auto node = rig.node("fresh.bin");
    TEST_CHECK(node != wb::kNoNode && nodeOf(before, fresh.location) == wb::kNoNode);
    rig.state.hover(WorkbenchPane::List, fresh);
    rig.view->setCursor(fresh);
    rig.frame();
    const auto& atlas = rig.view->instrument();
    TEST_CHECK(atlas.shownSnapshot().serial == before.snapshot->serial);
    TEST_CHECK(atlas.shapeView().marks().linked == wb::kNoNode && atlas.shapeView().marks().cursor == wb::kNoNode);
    TEST_CHECK(rig.view->ribbon().shapeView().marks().linked == node);
    // the older scene still names its own entries
    const auto note = nodeOf(before, rig.fixture.at("note.txt"));
    const auto circle = std::ranges::find_if(atlas.shapeView().shapes(), [&](const auto& shape) {
      return shape.id.node == note && shape.hittable && shape.kind == wb::ShapeKind::Circle;
    });
    TEST_CHECK(circle != atlas.shapeView().shapes().end());
    Point at;
    Node::absolutePosition(&atlas.shapeView(), at.x, at.y);
    rig.move({.x = at.x + circle->circle.center.x, .y = at.y + circle->circle.center.y});
    TEST_CHECK(rig.state.hovered() && rig.state.hovered()->entry.location == rig.fixture.at("note.txt"));
    rig.move(middle(*rig.list));
    rig.state.hover(WorkbenchPane::List, fresh);
    rig.until([&] { return atlas.shownSnapshot().serial == rig.model.serial(); });
    TEST_CHECK(atlas.shapeView().marks().linked == node && atlas.shapeView().marks().cursor == node);
  }

  // Once no part shows a snapshot the view lets go of it, a previous folder's included.
  void unshownSnapshotsAreReleased() {
    Rig rig;
    rig.settle();
    const std::weak_ptr<const WorkbenchSnapshot> first = rig.model.snapshot();
    rig.open("mid");
    rig.until([&] { return rig.model.snapshot()->location == rig.fixture.at("mid") && rig.loaded(); });
    TEST_CHECK(first.expired());
  }

  void fileTargetsPreview() {
    Rig rig;
    auto made = std::make_unique<InputArea>();
    const auto* preview = made.get();
    TEST_CHECK(rig.view->setPreview(std::move(made)) == nullptr);
    rig.settle();
    TEST_CHECK(rig.previews.empty() || !rig.previews.back().has_value());
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("note.txt"), .folder = false});
    rig.frame();
    rig.frame();
    TEST_CHECK(!rig.previews.empty());
    TEST_CHECK((rig.previews.back() == LookaheadPreview{.file = rig.fixture.at("note.txt"), .slot = 0}));
    TEST_CHECK(rig.view->center().column(0) == preview && shown(preview));
    const auto& plan = rig.view->center().plan();
    TEST_NEAR(preview->width(), plan.columns.back().x + plan.columns.back().width - plan.columns.front().x, 0.5);

    // a file row of column A previews in the slot of column B, which needs room for two columns
    rig.resize(1800, 900);
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true});
    rig.until([&] { return rig.columnRow(0, "b.bin") != nullptr; });
    TEST_CHECK(!rig.previews.back().has_value() && rig.view->center().column(0) == &rig.view->column(0));
    rig.click(*rig.columnRow(0, "b.bin"));
    rig.frame();
    TEST_CHECK((rig.previews.back() == LookaheadPreview{.file = rig.fixture.at("big/b.bin"), .slot = 1}));
    TEST_CHECK(rig.view->center().column(1) == preview && rig.columnRow(0, "b.bin")->marks().followed);
    // handing in another preview replaces it in place
    auto other = std::make_unique<InputArea>();
    const auto* next = other.get();
    TEST_CHECK(rig.view->setPreview(std::move(other)).get() == preview);
    rig.frame();
    TEST_CHECK(rig.view->center().column(1) == next);
  }

  void geometryRoundTrips() {
    Rig rig;
    WorkbenchGeometry saved;
    saved.panes.leftWidth = 400;
    saved.panes.ribbonHeights[0] = 260;
    saved.columns.columns = {300, 200, 150};
    rig.view->setGeometry(saved);
    rig.settle();
    TEST_CHECK(rig.geometries.empty() && rig.view->geometry() == saved);
    TEST_NEAR(rig.view->jumpList().width(), 400, 0.5);
    TEST_NEAR(rig.view->ribbon().height(), 260 - 24, 0.5);
    const auto left = middle(rig.view->shell().divider(wb::DividerId::Left));
    rig.dispatcher.pointerButton(left.x, left.y, BTN_LEFT, true, 10, 1000, false);
    rig.dispatcher.pointerMotion(left.x + 40, left.y, 11);
    rig.frame();
    rig.dispatcher.pointerButton(left.x + 40, left.y, BTN_LEFT, false, 12, 1100, false);
    rig.frame();
    TEST_CHECK(rig.geometries.size() == 1 && rig.geometries.back().panes.leftWidth == 440);
    TEST_CHECK(rig.geometries.back().columns == saved.columns && !rig.geometries.back().panes.columnWidths);
    // dragging a column's edge toward the list widens it
    const auto edge = middle(rig.view->center().divider(0));
    rig.dispatcher.pointerButton(edge.x, edge.y, BTN_LEFT, true, 13, 2000, false);
    rig.dispatcher.pointerMotion(edge.x - 20, edge.y, 14);
    rig.frame();
    rig.dispatcher.pointerButton(edge.x - 20, edge.y, BTN_LEFT, false, 15, 2100, false);
    rig.frame();
    TEST_CHECK(rig.geometries.size() == 2 && rig.geometries.back().columns.columns[0] == 320);
    TEST_CHECK(rig.geometries.back().panes.leftWidth == 440);
  }

  void paletteRolesBecomeSceneColors() {
    Palette roles{};
    roles.primary = rgba(1, 0, 0, 1);
    roles.surfaceContainerLow = rgba(0, 0.5F, 1, 1);
    roles.outlineVariant = rgba(0.2F, 0.4F, 0.6F, 1);
    const auto palette = workbenchPalette(roles);
    TEST_CHECK((palette.primary == wb::Rgb{.r = 255, .g = 0, .b = 0}));
    TEST_CHECK((palette.surfaceContainerLow == wb::Rgb{.r = 0, .g = 128, .b = 255}));
    TEST_CHECK((palette.outlineVariant == wb::Rgb{.r = 51, .g = 102, .b = 153}));
    TEST_CHECK(palette.categories == wb::ColorPalette{}.categories);
  }
} // namespace

int main() {
  everyPaneShowsTheTree();
  hoverLinksEveryPane();
  clicksGoWhereTheyPoint();
  clicksCarryButtonAndModifiers();
  doubleClickPinsThenOpens();
  headersSwitchKinds();
  panesCollapseExpandAndRestore();
  columnsFollowCursorRestAndPin();
  columnRowsRestAndPin();
  newStateLearnsTheCursor();
  partsMarkTheSnapshotTheyShow();
  unshownSnapshotsAreReleased();
  fileTargetsPreview();
  geometryRoundTrips();
  paletteRolesBecomeSceneColors();
  std::println("workbench-view: all checks passed");
  return 0;
}
