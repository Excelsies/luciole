#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "metrics_renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/browser/status_pill.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"
#include "workbench/pane_layout.h"

#include <algorithm>
#include <cstddef>
#include <glib.h>
#include <memory>
#include <optional>
#include <print>
#include <wayland-client-protocol.h>

// Where the page's status pill floats: the file pane's place in the page body while the Workbench view shows the
// pane, and none while the expanded instrument covers it. A slide of the pane moves its drawing, not its place. The
// pill takes the bottom of that place, below the list's rows, so it never covers the row under the cursor.
namespace {
  LayoutRect drawnAt(const Node& node) {
    LayoutRect rect{.width = node.width(), .height = node.height()};
    Node::absolutePosition(&node, rect.x, rect.y);
    return rect;
  }

  void paneAreaIsThePanesPlace() {
    luciole::test::TempFolder home{"luciole-workbench-pane-area"};
    for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
      g_setenv(variable, home.path().c_str(), TRUE);
    }
    luciole::WorkbenchServices services;
    luciole::FileIconCache icons;
    ::test::MetricsRenderer renderer;
    Node body; // the page body: the pane between two siblings
    body.addChild(std::make_unique<Node>());
    auto made = std::make_unique<Node>();
    made->setFlexGrow(1);
    auto* focus = static_cast<InputArea*>(made->addChild(std::make_unique<InputArea>()));
    auto* pane = body.addChild(std::move(made));
    body.addChild(std::make_unique<Node>());
    luciole::WorkbenchSession session(
        *pane, focus, services.measureCache(), icons, [] { return 0U; },
        luciole::WorkbenchModelOptions{.ancestorLevels = 0}
    );
    session.update({}, {});
    const auto layout = [&] {
      for (std::size_t round = 0; round < 2; ++round) { // the view plans its regions, then places them
        session.view().arrange(renderer, {40, 30, 1400, 900});
      }
    };
    layout();
    body.setPosition(200, 90);
    const auto place = drawnAt(*pane);
    TEST_CHECK(place.width > 300 && place.height > 300);
    TEST_CHECK(place.x > 240 && place.x + place.width < 1640);
    const auto area = session.paneArea(body);
    TEST_CHECK(area.has_value());
    TEST_NEAR(area->x, place.x - 200, 1e-3);
    TEST_NEAR(area->y, place.y - 90, 1e-3);
    TEST_NEAR(area->width, place.width, 1e-3);
    TEST_NEAR(area->height, place.height, 1e-3);
    std::println("pane area {}, {} {}x{}", area->x, area->y, area->width, area->height);

    pane->setTranslate(24, 0); // a change of folder slides the pane in
    TEST_NEAR(drawnAt(*pane).x, place.x + 24, 1e-3);
    const auto sliding = session.paneArea(body);
    TEST_CHECK(sliding.has_value());
    TEST_NEAR(sliding->x, place.x - 200, 1e-3);
    pane->setTranslate(0, 0);

    auto& shell = session.view().shell();
    shell.setState(luciole::workbench::withExpanded(shell.state(), true));
    layout();
    TEST_CHECK(!session.paneArea(body));
    shell.setState(luciole::workbench::withExpanded(shell.state(), false));
    layout();
    TEST_CHECK(session.paneArea(body).has_value());
  }

  constexpr std::size_t kRows = 40;
  constexpr float kRowHeight = 36; // as the page's list

  class Rows final : public VirtualGridAdapter {
  public:
    std::size_t itemCount() const override { return kRows; }
    std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}
  };

  void setHome(const luciole::test::TempFolder& home) {
    for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_DATA_DIRS"}) {
      g_setenv(variable, home.path().c_str(), TRUE);
    }
  }

  // The page's file pane in the page body, with the list's grid in it and the status pill beside it, the session
  // mounted on the pane with the pill's room kept.
  struct PillRig {
    luciole::test::TempFolder home{"luciole-workbench-pane-pill"};
    luciole::WorkbenchServices services;
    luciole::FileIconCache icons;
    ::test::MetricsRenderer renderer;
    Rows rows;
    Node body;
    Flex* pane = nullptr;
    VirtualGridView* list = nullptr;
    StatusPill pill;
    std::optional<luciole::WorkbenchSession> session;
    InputDispatcher dispatcher;

    PillRig() {
      setHome(home);
      body.setSize(1100, 700);
      pane = static_cast<Flex*>(body.addChild(
          ui::column(
              {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .padding = Style::spaceSm, .flexGrow = 1.0F},
              ui::virtualGridView(
                  {.out = &list,
                   .columns = 1,
                   .cellHeight = kRowHeight,
                   .squareCells = false,
                   .columnGap = 0.0F,
                   .rowGap = 2.0F,
                   .adapter = &rows,
                   .flexGrow = 1.0F}
              )
          )
      ));
      body.addChild(buildStatusPill(pill));
      pill.pill->setVisible(true);
      (void)pill.status->setText("23 items · 1 selected · 49.3 KiB");
      session.emplace(
          *pane, list->focusArea(), services.measureCache(), icons, [] { return 0U; },
          luciole::WorkbenchModelOptions{.ancestorLevels = 0}
      );
      session->keepPillRoom(*list);
      session->update({}, {});
      dispatcher.setSceneRoot(&body);
      layout();
    }
    PillRig(const PillRig&) = delete;
    PillRig& operator=(const PillRig&) = delete;
    PillRig(PillRig&&) = delete;
    PillRig& operator=(PillRig&&) = delete;
    ~PillRig() { session.reset(); } // before the body it took the pane from

    // The page's frame: the view, then the pill in the place paneArea() gives.
    void layout() {
      for (std::size_t round = 0; round < 2; ++round) { // the view plans its regions, then places them
        session->view().arrange(renderer, {0, 0, 1100, 700});
      }
      const auto area = session->paneArea(body);
      TEST_CHECK(area.has_value());
      arrangeStatusPill(renderer, pill, area.value_or(LayoutRect{}), true);
    }
  };

  // The cursor walks down the list as the Down key moves it; every row it reaches is shown whole and above the
  // shown status pill, which floats in the place paneArea() gives.
  void cursorRowStaysAboveThePill() {
    PillRig rig;
    const auto pillRect = drawnAt(*rig.pill.pill);
    TEST_CHECK(pillRect.width > 100 && pillRect.height >= Style::controlHeightSm);
    const auto pane = drawnAt(*rig.pane);
    TEST_CHECK(pillRect.y + pillRect.height <= pane.y + pane.height); // the pill floats over the pane's own room
    for (std::size_t cursor = 0; cursor < kRows; ++cursor) {
      rig.list->setSelectedIndex(cursor, true);
      rig.layout();
      const auto row = rig.list->itemRect(cursor);
      TEST_CHECK(row.has_value());
      if (!row) {
        continue;
      }
      TEST_NEAR(row->height, kRowHeight, 1e-3); // shown whole
      TEST_CHECK(row->y + row->height <= drawnAt(*rig.pill.pill).y);
    }
    TEST_CHECK(rig.list->scrollView().scrollOffset() > 0);
  }

  // Beside the pill, below the list's rows, the room is the pane's: a drop there finds the pane and no row under it,
  // so it goes to the folder, and a wheel turned there scrolls the list.
  void roomBesideThePillIsThePanes() {
    PillRig rig;
    const auto pillRect = drawnAt(*rig.pill.pill);
    const auto grid = drawnAt(*rig.list);
    const float x = drawnAt(*rig.pane).x + 40;
    const float y = pillRect.y + (pillRect.height / 2);
    TEST_CHECK(x < pillRect.x && y > grid.y + grid.height);
    TEST_CHECK(rig.pane->containsScenePoint(x, y));
    TEST_CHECK(!rig.list->itemAtScene(x, y));
    TEST_CHECK(rig.list->scrollView().scrollOffset() == 0);
    rig.dispatcher.pointerEnter(x, y, 1);
    TEST_CHECK(
        rig.dispatcher.pointerAxis(x, y, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 15, 1, 120, 1)
    );
    TEST_CHECK(rig.list->scrollView().scrollOffset() > 0);
    const float down = rig.list->scrollView().scrollOffset();
    TEST_CHECK(rig.dispatcher.pointerAxis(
        x, y, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, -15, -1, -120, -1
    ));
    TEST_CHECK(rig.list->scrollView().scrollOffset() < down);
    // sideways turns are the page's, for the details columns
    TEST_CHECK(!rig.dispatcher.pointerAxis(
        x, y, WL_POINTER_AXIS_HORIZONTAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 15, 1, 120, 1
    ));
  }
} // namespace

int main() {
  paneAreaIsThePanesPlace();
  cursorRowStaysAboveThePill();
  roomBesideThePillIsThePanes();
  std::println("workbench pane area checks passed");
  return 0;
}
