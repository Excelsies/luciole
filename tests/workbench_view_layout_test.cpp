#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/segmented.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench_view_rig.h"

#include <array>
#include <cstddef>
#include <format>
#include <print>

namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;

  struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
  };

  Rect box(const Node* node) {
    TEST_CHECK(node != nullptr);
    Rect rect{.width = node->width(), .height = node->height()};
    Node::absolutePosition(node, rect.x, rect.y);
    return rect;
  }

  std::string title(const Rig& rig, wb::Pane pane) { return rig.view->shell().header(pane).titleLabel()->text(); }

  // The left column takes about 22% of the view between 340 and 520, the flame ribbon its 224 px over the center,
  // and the columns ahead drop from the right before the list goes under 320.
  void laysOutByTheContract() {
    Rig rig;
    rig.settle();
    struct Size {
      float width;
      float height;
      float left;
    };
    for (const auto& size : {Size{1100, 700, 340}, Size{1400, 900, 340}, Size{2560, 1400, 520}}) {
      rig.resize(size.width, size.height);
      const auto& shell = rig.view->shell();
      const auto instrument = box(shell.content(PaneRegion::Instrument));
      TEST_CHECK(instrument.x == 0 && instrument.y == 24 && instrument.width == size.left);
      const auto jump = box(shell.content(PaneRegion::Jump));
      TEST_CHECK(jump.x == 0 && jump.width == size.left && jump.y + jump.height == size.height);
      TEST_CHECK(jump.y > instrument.y + instrument.height);
      const auto ribbon = box(shell.content(PaneRegion::Ribbon));
      TEST_CHECK(ribbon.x == size.left && ribbon.y == 24 && ribbon.height == 224);
      TEST_CHECK(ribbon.width == size.width - size.left);
      const auto center = box(shell.content(PaneRegion::Center));
      TEST_CHECK(center.x == size.left && center.y == 248 && center.width == size.width - size.left);
      TEST_CHECK(center.y + center.height == size.height);
      const auto& plan = rig.view->center().plan();
      TEST_CHECK(plan.list.width >= 320 && !plan.columns.empty());
      for (std::size_t slot = 0; slot < wb::kColumnsAhead; ++slot) {
        const auto* column = &rig.view->column(slot);
        TEST_CHECK(shown(column) == (slot < plan.columns.size()));
        if (slot < plan.columns.size()) {
          TEST_NEAR(column->width(), plan.columns[slot].width, 0.5);
          TEST_NEAR(box(column).x, center.x + plan.columns[slot].x, 0.5);
        }
      }
      std::println("{}x{}: list {}, {} columns", size.width, size.height, plan.list.width, plan.columns.size());
      if (size.width == 2560) {
        TEST_CHECK(plan.columns.size() == 3);
      }
    }
    TEST_CHECK(rig.geometries.empty());
  }

  // The ribbon closes first, then Jump Ahead, then the left column turns into the rail.
  void smallViewsClosePanesInOrder() {
    Rig rig;
    rig.settle();
    const auto& shell = rig.view->shell();
    rig.resize(1400, 340);
    TEST_CHECK(title(rig, wb::Pane::Ribbon) == "Ribbon: Flame Graph (closed: too short)");
    TEST_CHECK(title(rig, wb::Pane::Jump) == "Jump Ahead");
    TEST_CHECK(!shown(&rig.view->ribbon()) && shown(&rig.view->jumpList()));
    rig.resize(1400, 300);
    TEST_CHECK(title(rig, wb::Pane::Jump) == "Jump Ahead (closed: too short)");
    TEST_CHECK(!shown(&rig.view->jumpList()) && shown(&rig.view->instrument()));
    rig.resize(540, 700);
    TEST_CHECK(shown(shell.railButton(wb::Pane::Instrument)) && !shown(&rig.view->instrument()));
    TEST_CHECK(shown(&rig.view->center()));
    rig.resize(1400, 900);
    TEST_CHECK(shown(&rig.view->instrument()) && shown(&rig.view->jumpList()) && shown(&rig.view->ribbon()));
    TEST_CHECK(title(rig, wb::Pane::Ribbon) == "Ribbon: Flame Graph" && rig.geometries.empty());
  }

  // Frames with nothing new lay no scene out and bind no row; a hover binds only the row it marks.
  void idleFramesPushNothing() {
    Rig rig;
    rig.settle();
    auto serial = rig.model.serial();
    auto since = Clock::now();
    rig.until([&] {
      if (rig.model.serial() != serial) {
        serial = rig.model.serial();
        since = Clock::now();
      }
      const auto status = rig.services.measureService().status();
      return status.queued + status.running == 0 && Clock::now() - since > 3 * WorkbenchModel::kPublishInterval;
    });
    rig.settle();
    std::size_t binds = 0;
    for (std::size_t slot = 0; slot < wb::kColumnsAhead; ++slot) {
      auto& column = rig.view->column(slot);
      column.share = [&binds, read = column.share](EntryId id) {
        ++binds;
        return read ? read(id) : std::nullopt;
      };
    }
    const auto builds = [&rig] { return rig.view->instrument().sceneBuilds() + rig.view->ribbon().sceneBuilds(); };
    const auto before = builds();
    for (int frame = 0; frame < 10; ++frame) {
      rig.frame();
    }
    TEST_CHECK(builds() == before && binds == 0);
    rig.state.hover(WorkbenchPane::List, {.location = rig.fixture.at("big/deep"), .folder = true});
    rig.frame();
    TEST_CHECK(builds() == before && binds == 1);
    TEST_CHECK(rig.columnRow(0, "deep")->marks().linked);
  }

  // The view goes while a folder is measured and listed and the instrument tweens to another kind.
  void destroyedWhileLoadingAndMoving() {
    Rig rig;
    for (int folder = 0; folder < 40; ++folder) {
      const auto path = rig.fixture.base / "wide" / std::format("d{:02}", folder);
      fs::create_directories(path);
      for (int file = 0; file < 100; ++file) {
        std::ofstream(path / std::format("f{:03}", file));
      }
    }
    rig.settle();
    rig.click(*visible<Button>(rig.view->instrumentSwitch())[1]);
    rig.frame();
    rig.frame();
    TEST_CHECK(rig.animations.hasActive());
    rig.open(rig.fixture.base / "wide");
    rig.frame();
    TEST_CHECK(rig.model.measuring());
    auto view = rig.root.removeChild(rig.view);
    rig.view = nullptr;
    view.reset();
    TEST_CHECK(!rig.animations.hasActive());
    const auto end = Clock::now() + std::chrono::milliseconds(300);
    while (Clock::now() < end) {
      rig.pump.once();
    }
  }
} // namespace

int main() {
  laysOutByTheContract();
  smallViewsClosePanesInOrder();
  idleFramesPushNothing();
  destroyedWhileLoadingAndMoving();
  std::println("workbench-view-layout: all checks passed");
  return 0;
}
