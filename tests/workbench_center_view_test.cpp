#include "core/input/key_modifiers.h"
#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/pane_divider.h"
#include "ui/style.h"
#include "ui/workbench/ancestor_strips.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/column_plan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <print>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  std::size_t g_allocations = 0;
}

void* operator new(std::size_t size) {
  ++g_allocations;
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
  using luciole::AncestorEntry;
  using luciole::WorkbenchCenterView;
  using luciole::workbench::ColumnWidths;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  const ColumnWidths kDefaults{.columns = {260, 190, 140}, .list = std::nullopt};

  struct SceneRect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };

  SceneRect sceneBox(const Node* node) {
    TEST_CHECK(node != nullptr);
    SceneRect box{.width = node->width(), .height = node->height()};
    Node::absolutePosition(node, box.x, box.y);
    return box;
  }

  bool shown(const Node* node) {
    for (; node != nullptr; node = node->parent()) {
      if (!node->visible()) {
        return false;
      }
    }
    return true;
  }

  // A view filling a scene, three ancestors in a strips pane as wide as their bands (107 px), a list whose input area
  // sits at a grid's z (50) and three columns.
  struct Rig {
    test::MetricsRenderer renderer;
    Node root;
    WorkbenchCenterView* view = nullptr;
    InputArea* list = nullptr;
    std::array<InputArea*, 3> columns{};
    InputDispatcher dispatcher;
    std::vector<ColumnWidths> reports;
    int listPresses = 0;
    std::uint32_t serial = 0;
    float width = 1400;
    float height = 600;

    explicit Rig(float viewWidth = 1400) : width(viewWidth) {
      root.setSize(4000, 4000);
      view = static_cast<WorkbenchCenterView*>(root.addChild(std::make_unique<WorkbenchCenterView>()));
      auto area = std::make_unique<InputArea>();
      area->setZIndex(50);
      area->setOnPress([this](const InputArea::PointerData& data) { listPresses += data.pressed ? 1 : 0; });
      list = area.get();
      TEST_CHECK(view->setList(std::move(area)) == nullptr);
      for (std::size_t i = 0; i < columns.size(); ++i) {
        auto column = std::make_unique<InputArea>();
        columns[i] = column.get();
        TEST_CHECK(view->setColumn(i, std::move(column)) == nullptr);
      }
      view->setAncestors({
          {.name = "c", .path = "/a/b/c", .id = 1},
          {.name = "b", .path = "/a/b", .id = 2},
          {.name = "a", .path = "/a", .id = 3},
      });
      view->setStripsWidth(107);
      view->setOnWidths([this](const ColumnWidths& widths) { reports.push_back(widths); });
      dispatcher.setSceneRoot(&root);
      layout();
    }

    void layout() { view->arrange(renderer, {0, 0, width, height}); }
    void resize(float w) {
      width = w;
      layout();
    }
    SceneRect column(std::size_t i) const { return sceneBox(columns[i]); }
    SceneRect listBox() const { return sceneBox(list); }
    std::size_t shownColumns() const {
      std::size_t count = 0;
      for (std::size_t i = 0; i < columns.size(); ++i) {
        TEST_CHECK(shown(columns[i]) == (i < view->plan().columns.size()));
        count += shown(columns[i]) ? 1 : 0;
      }
      return count;
    }

    void press(float x, float y = 300, std::uint32_t time = 1000) {
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, time, false);
    }
    // One frame of a drag: the pointer moves, then the window lays out.
    void move(float x, float y = 300) {
      dispatcher.pointerMotion(x, y, ++serial);
      layout();
    }
    void release(float x, float y = 300, std::uint32_t time = 1100) {
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, time, false);
      layout();
    }
    void key(std::uint32_t sym, std::uint32_t modifiers = 0) {
      dispatcher.keyEvent(sym, 0, modifiers, true);
      dispatcher.keyEvent(sym, 0, modifiers, false);
      layout();
    }
  };

  // Three ancestors take 26 + 34 + 44 px and a 1 px gap after each: 107.
  void planNumbersAtFourWidths() {
    Rig rig(2560);
    const auto strips = sceneBox(&rig.view->strips());
    TEST_CHECK(strips.x == 294 && strips.width == 107); // the list stops at 1272 and the group is centered
    TEST_CHECK(sceneBox(rig.view->strips().area(2)).x == 356 && sceneBox(rig.view->strips().area(2)).width == 44);
    TEST_CHECK(rig.listBox().x == 401 && rig.listBox().width == 1272);
    TEST_CHECK(rig.column(0).x == 1673 && rig.column(0).width == 260);
    TEST_CHECK(rig.column(1).x == 1934 && rig.column(1).width == 190);
    TEST_CHECK(rig.column(2).x == 2125 && rig.column(2).width == 140);
    TEST_CHECK(rig.column(2).y == 0 && rig.column(2).height == 600);
    const auto divider = sceneBox(&rig.view->divider(0));
    TEST_CHECK(divider.x == 1670 && divider.width == 6 && divider.height == 600);

    rig.resize(1400);
    TEST_CHECK(sceneBox(&rig.view->strips()).x == 0);
    TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == 700);
    TEST_CHECK(rig.column(0).x == 807 && rig.column(1).x == 1068 && rig.column(2).x == 1259);
    TEST_CHECK(rig.column(2).x + rig.column(2).width == 1399);
    TEST_CHECK(rig.shownColumns() == 3);

    rig.resize(1100); // a third column would leave the list under 470
    TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == 541);
    TEST_CHECK(rig.column(0).x == 648 && rig.column(0).width == 260);
    TEST_CHECK(rig.column(1).x == 909 && rig.column(1).width == 190);
    TEST_CHECK(rig.shownColumns() == 2);
    TEST_CHECK(!shown(&rig.view->divider(2)) && shown(&rig.view->divider(1)));

    rig.resize(700); // the first column needs only the list's 320
    TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == 332);
    TEST_CHECK(rig.column(0).x == 439 && rig.column(0).width == 260);
    TEST_CHECK(rig.shownColumns() == 1);
    TEST_CHECK(rig.reports.empty());
  }

  void stripsOffGiveTheirWidthToTheList() {
    Rig rig;
    rig.view->setStripsShown(false);
    rig.layout();
    TEST_CHECK(!shown(&rig.view->strips()));
    TEST_CHECK(rig.listBox().x == 0 && rig.listBox().width == 807);
    TEST_CHECK(rig.column(0).x == 807);
    rig.view->setStripsShown(true);
    rig.layout();
    TEST_CHECK(shown(&rig.view->strips()) && rig.view->strips().count() == 3);
    TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == 700);
  }

  // Without ancestors the pane is empty and keeps its width, and so does a deeper folder: the list stays put.
  void paneKeepsItsWidthWithoutAncestors() {
    Rig rig;
    rig.view->setStripsWidth(std::nullopt);
    rig.layout();
    TEST_CHECK(rig.listBox().x == 153 && rig.view->strips().count() == 3);
    rig.view->setAncestors({});
    rig.layout();
    TEST_CHECK(rig.view->strips().count() == 0 && rig.view->plan().stripsWidth == 153);
    TEST_CHECK(rig.listBox().x == 153 && shown(&rig.view->stripsDivider()));
    std::vector<AncestorEntry> deep;
    for (std::uint64_t id = 1; id <= 30; ++id) {
      deep.push_back({.name = std::to_string(id), .path = "/", .id = id});
    }
    rig.view->setAncestors(deep);
    rig.layout();
    TEST_CHECK(rig.listBox().x == 153 && rig.view->strips().count() == 5); // four named and "+26 levels"
    TEST_CHECK(rig.reports.empty());
  }

  // The list, the strips, the columns and the dividers all take the view's height.
  void regionsTakeTheViewsHeight() {
    Rig rig;
    rig.layout();
    TEST_CHECK(rig.listBox().y == 0 && rig.listBox().height == 600);
    TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == 700);
    TEST_CHECK(sceneBox(&rig.view->strips()).height == 600 && rig.column(0).height == 600);
    TEST_CHECK(sceneBox(&rig.view->divider(0)).height == 600);
    TEST_CHECK(rig.reports.empty());
  }

  void columnsDropAndReturnInOrder() {
    Rig rig;
    const std::array<float, 7> widths{1400, 1100, 700, 600, 700, 1100, 1400};
    const std::array<std::size_t, 7> counts{3, 2, 1, 0, 1, 2, 3};
    for (std::size_t i = 0; i < widths.size(); ++i) {
      rig.resize(widths[i]);
      TEST_CHECK(rig.shownColumns() == counts[i]);
      TEST_CHECK(rig.listBox().width >= 320);
    }
    TEST_CHECK(rig.listBox().width == 700 && rig.column(2).x == 1259);
  }

  void slowNarrowingDragFollowsThePointer() {
    Rig rig(1100);
    rig.press(648);
    TEST_CHECK(rig.view->resizing());
    for (int k = 1; k <= 30; ++k) {
      rig.move(static_cast<float>(648 + 5 * k));
      // the first 5 px are under the drag threshold; then the edge follows until the column is 140 wide
      const float edge = k == 1 ? 648.0F : std::min(648.0F + 5.0F * static_cast<float>(k), 768.0F);
      TEST_CHECK(rig.column(0).x == edge);
      TEST_CHECK(rig.column(0).width == 260 - (edge - 648));
      TEST_CHECK(rig.listBox().x == 107 && rig.listBox().width == edge - 107);
      TEST_CHECK(rig.shownColumns() == 2); // the count it started with, though a third would fit
    }
    TEST_CHECK(rig.reports.empty());
    rig.release(798);
    TEST_CHECK(rig.reports.size() == 1);
    TEST_CHECK((rig.reports[0] == ColumnWidths{.columns = {140, 190, 140}, .list = std::nullopt}));
    TEST_CHECK(!rig.view->resizing());
    TEST_CHECK(rig.shownColumns() == 3); // planned again on release
    TEST_CHECK(rig.listBox().width == 520 && rig.column(0).x == 627);
  }

  void wideningDragStopsAtTheListFloor() {
    Rig rig(1100);
    rig.press(648);
    for (int k = 1; k <= 20; ++k) {
      rig.move(static_cast<float>(648 - 20 * k));
    }
    TEST_CHECK(rig.column(0).width == 481); // 260 plus all the list could give
    TEST_CHECK(rig.listBox().width == 320 && rig.column(0).x == 427);
    rig.release(248);
    TEST_CHECK(rig.reports.size() == 1);
    TEST_CHECK((rig.reports[0] == ColumnWidths{.columns = {481, 190, 140}, .list = 320.0}));
    TEST_CHECK(rig.shownColumns() == 2 && rig.listBox().width == 320);
  }

  // The window narrows while the button is held, before the pointer moves: the drag still starts from the edge
  // the divider was pressed on.
  void relayoutBetweenPressAndMotion() {
    Rig rig;
    rig.press(807);
    rig.resize(1300);
    TEST_CHECK(rig.column(0).x == 707 && rig.column(0).width == 260);
    rig.move(817); // 10 px toward the far edge
    TEST_CHECK(rig.column(0).width == 250 && rig.column(0).x == 717);
    TEST_CHECK(rig.shownColumns() == 3);
    rig.release(817);
    TEST_CHECK(rig.reports.size() == 1 && rig.reports[0].columns[0] == 250);
  }

  void keysResetAndEscape() {
    Rig rig;
    rig.dispatcher.setFocus(&rig.view->divider(0));
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.reports.size() == 1 && rig.reports[0].columns[0] == 276);
    TEST_CHECK(rig.column(0).x == 791 && rig.column(0).width == 276);
    rig.key(XKB_KEY_Left, KeyMod::Shift);
    TEST_CHECK(rig.column(0).width == 340 && rig.column(0).x == 727);
    rig.key(XKB_KEY_Right);
    TEST_CHECK(rig.column(0).width == 324);
    rig.key(XKB_KEY_Home);
    TEST_CHECK(rig.column(0).width == 260 && rig.column(0).x == 807);
    TEST_CHECK(rig.reports.size() == 4 && rig.reports[3] == kDefaults);
    // a double press resets one divider
    rig.dispatcher.setFocus(&rig.view->divider(1));
    rig.key(XKB_KEY_Left);
    TEST_CHECK(rig.column(1).width == 206 && rig.column(1).x == 1052);
    rig.dispatcher.setFocus(nullptr);
    rig.press(1052, 300, 1000);
    rig.release(1052, 300, 1050);
    rig.press(1053, 302, 1200);
    rig.release(1053, 302, 1250);
    TEST_CHECK(rig.reports.size() == 6 && rig.reports[5] == kDefaults);
    TEST_CHECK(rig.column(1).width == 190 && rig.column(1).x == 1068);
    // Escape during a drag restores the widths it started from and reports nothing
    rig.press(1259);
    rig.move(1200);
    TEST_CHECK(rig.column(2).x == 1200 && rig.column(2).width == 199);
    rig.view->setWidths({.columns = {300, 300, 300}, .list = std::nullopt}); // ignored while dragging
    rig.key(XKB_KEY_Escape);
    TEST_CHECK(rig.column(2).x == 1259 && rig.column(2).width == 140);
    TEST_CHECK(!rig.view->resizing() && rig.view->widths() == kDefaults);
    rig.move(1150);
    rig.release(1150);
    TEST_CHECK(rig.column(2).x == 1259);
    // so does a lost pointer capture
    rig.press(807);
    rig.move(760);
    TEST_CHECK(rig.column(0).width == 307);
    rig.dispatcher.cancelPointerCapture();
    rig.layout();
    TEST_CHECK(rig.column(0).width == 260);
    TEST_CHECK(rig.reports.size() == 6);
  }

  void savedWidthsRoundTrip() {
    Rig rig;
    const ColumnWidths saved{.columns = {300, 200, 150}, .list = std::nullopt};
    rig.view->setWidths(saved);
    rig.layout();
    TEST_CHECK(rig.reports.empty() && rig.view->widths() == saved);
    TEST_CHECK(rig.column(0).x == 747 && rig.column(0).width == 300);
    TEST_CHECK(rig.column(1).x == 1048 && rig.column(1).width == 200);
    TEST_CHECK(rig.column(2).x == 1249 && rig.column(2).width == 150);
    rig.dispatcher.setFocus(&rig.view->divider(2));
    rig.key(XKB_KEY_Right); // 150 - 16 stops at 140
    TEST_CHECK(rig.reports.size() == 1);
    TEST_CHECK((rig.reports[0] == ColumnWidths{.columns = {300, 200, 140}, .list = std::nullopt}));
    rig.view->setWidths({.columns = {kNan, 50, 700}, .list = 100.0});
    TEST_CHECK(rig.view->widths() == kDefaults);
    rig.view->setWidths({.columns = {260, 190, 140}, .list = 400.4});
    TEST_CHECK(rig.view->widths().list == 400.0);
    rig.layout();
    TEST_CHECK(rig.reports.size() == 1);
  }

  void dividersSitAboveTheList() {
    Rig rig;
    rig.press(805); // inside the list, within the divider's 6 px
    TEST_CHECK(rig.view->resizing());
    rig.release(805);
    TEST_CHECK(rig.listPresses == 0);
    rig.press(700);
    TEST_CHECK(rig.listPresses == 1);
    rig.release(700);
  }

  void draggedDividerStaysDuringItsDrag() {
    Rig rig;
    auto& divider = rig.view->divider(2);
    rig.press(1259);
    rig.move(1250);
    rig.resize(700); // the third column no longer fits
    TEST_CHECK(shown(&divider) && divider.parent() == rig.view);
    TEST_CHECK(rig.dispatcher.pointerCaptured() && divider.dragging());
    TEST_CHECK(!shown(rig.columns[2]));
    rig.release(1250);
    TEST_CHECK(rig.reports.size() == 1 && rig.reports[0].columns[2] == 149);
    TEST_CHECK(!shown(&divider));
  }

  void previewSpansToTheFarEdge() {
    Rig rig;
    rig.view->setSpanFrom(1);
    rig.layout();
    TEST_CHECK(rig.column(1).x == 1068 && rig.column(1).width == 331);
    TEST_CHECK(!shown(rig.columns[2]) && !shown(&rig.view->divider(2)));
    TEST_CHECK(shown(&rig.view->divider(1)) && shown(rig.columns[0]));
    rig.view->setSpanFrom(0);
    rig.layout();
    TEST_CHECK(rig.column(0).x == 807 && rig.column(0).width == 592);
    TEST_CHECK(!shown(rig.columns[1]) && !shown(&rig.view->divider(1)));
    rig.view->setSpanFrom(2);
    rig.resize(1100); // its slot is not planned: the planned columns keep their own room
    TEST_CHECK(rig.column(1).width == 190 && !shown(rig.columns[2]));
    rig.view->setSpanFrom(std::nullopt);
    rig.resize(1400);
    TEST_CHECK(rig.shownColumns() == 3 && rig.column(2).width == 140);
  }

  void contentChangesHands() {
    Rig rig;
    auto replacement = std::make_unique<InputArea>();
    auto* raw = replacement.get();
    const auto previous = rig.view->setList(std::move(replacement));
    TEST_CHECK(previous.get() == rig.list && previous->parent() == nullptr);
    TEST_CHECK(rig.view->list() == raw);
    rig.resize(700);
    const auto hidden = rig.view->setColumn(2, nullptr);
    TEST_CHECK(hidden.get() == rig.columns[2] && hidden->visible());
    TEST_CHECK(rig.view->column(2) == nullptr);
    auto extra = std::make_unique<InputArea>();
    auto* extraRaw = extra.get();
    TEST_CHECK(rig.view->setColumn(3, std::move(extra)).get() == extraRaw);
    TEST_CHECK(rig.view->column(3) == nullptr);
    rig.layout();
    TEST_CHECK(sceneBox(raw).x == 107 && sceneBox(raw).width == 332);
  }

  void rightToLeftMirrors() {
    Style::setRtl(true);
    {
      Rig rig;
      TEST_CHECK(rig.listBox().x == 593 && rig.listBox().width == 700);
      TEST_CHECK(rig.column(0).x == 333 && rig.column(2).x == 1);
      TEST_CHECK(sceneBox(&rig.view->strips()).x == 1293);
      TEST_CHECK(sceneBox(rig.view->strips().area(2)).x == 1294); // the nearest strip beside the list
      TEST_CHECK(sceneBox(&rig.view->divider(0)).x == 590);
      rig.press(593);
      rig.move(573); // toward the column: it narrows
      TEST_CHECK(rig.column(0).width == 240 && rig.column(0).x + rig.column(0).width == 573);
      rig.release(573);
      TEST_CHECK(rig.reports.size() == 1 && rig.reports[0].columns[0] == 240);
      rig.dispatcher.setFocus(&rig.view->divider(0));
      rig.key(XKB_KEY_Left); // the edge moves left on screen
      TEST_CHECK(rig.view->widths().columns[0] == 224);
      TEST_CHECK(rig.column(0).x + rig.column(0).width == 557);
    }
    Style::setRtl(false);
  }

  void unchangedLayoutAllocatesNothing() {
    Rig rig;
    rig.layout();
    g_allocations = 0;
    for (int i = 0; i < 100; ++i) {
      rig.layout();
    }
    std::println("center view layout allocations: {} in 100 unchanged passes", g_allocations);
    TEST_CHECK(g_allocations == 0);
  }
} // namespace

int main() {
  planNumbersAtFourWidths();
  stripsOffGiveTheirWidthToTheList();
  paneKeepsItsWidthWithoutAncestors();
  regionsTakeTheViewsHeight();
  columnsDropAndReturnInOrder();
  slowNarrowingDragFollowsThePointer();
  wideningDragStopsAtTheListFloor();
  relayoutBetweenPressAndMotion();
  keysResetAndEscape();
  savedWidthsRoundTrip();
  dividersSitAboveTheList();
  draggedDividerStaysDuringItsDrag();
  previewSpansToTheFarEdge();
  contentChangesHands();
  rightToLeftMirrors();
  unchangedLayoutAllocatesNothing();
  std::println("workbench center view checks passed");
}
