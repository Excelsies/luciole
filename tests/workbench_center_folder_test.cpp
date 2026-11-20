#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/workbench_center_view.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The center's folder: the strips it gives the folders above, the motion of a change of folder, and where the
// keyboard focus goes after a strip's key.
namespace {
  using luciole::CenterMotion;
  using luciole::Location;
  using luciole::WorkbenchCenterView;

  constexpr float kSlide = WorkbenchCenterView::kMotionSlide;

  Location at(const std::string& path) { return Location::fromPath(path); }

  struct Rig {
    test::MetricsRenderer renderer;
    AnimationManager animations;
    Node root;
    Node* holder = nullptr;
    WorkbenchCenterView* view = nullptr;
    InputArea* list = nullptr;
    std::array<InputArea*, 3> columns{};
    InputDispatcher dispatcher;
    std::vector<std::pair<Location, std::uint32_t>> asked;
    std::uint32_t serial = 0;

    Rig() {
      root.setSize(4000, 4000);
      root.setAnimationManager(&animations);
      holder = root.addChild(std::make_unique<Node>());
      view = static_cast<WorkbenchCenterView*>(holder->addChild(std::make_unique<WorkbenchCenterView>()));
      auto made = std::make_unique<InputArea>();
      made->setFocusable(true);
      list = made.get();
      TEST_CHECK(view->setList(std::move(made)) == nullptr);
      for (std::size_t slot = 0; slot < columns.size(); ++slot) {
        auto column = std::make_unique<InputArea>();
        columns[slot] = column.get();
        TEST_CHECK(view->setColumn(slot, std::move(column)) == nullptr);
      }
      view->onStripActivate = [this](const Location& folder, std::uint32_t button) {
        asked.emplace_back(folder, button);
      };
      dispatcher.setSceneRoot(&root);
    }

    // A frame: the window lays out, draws and clears its dirty marks.
    void layout() {
      view->arrange(renderer, {0, 0, 1400, 800});
      root.clearDirty();
    }

    // Ticks the motion, which runs on the clock, to its end; fails after two seconds.
    void settle() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (view->moving()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const float before = std::abs(list->translateX());
        animations.tick(16);
        TEST_CHECK(!root.layoutDirty()); // the motion lays nothing out
        TEST_CHECK(std::abs(list->translateX()) <= before);
        root.clearDirty();
      }
    }
  };

  Color stripFill(const InputArea& strip) {
    for (const auto& child : strip.children()) {
      if (const auto* box = dynamic_cast<const Box*>(child.get())) {
        return box->style().fill;
      }
    }
    TEST_CHECK(false);
    return {};
  }

  bool atRest(const Node& node) { return node.translateX() == 0 && node.translateY() == 0 && node.opacity() == 1; }

  void directionsOfAChange() {
    TEST_CHECK(luciole::centerMotion(at("/a/b"), at("/a/b/c/d")) == CenterMotion::Deeper);
    TEST_CHECK(luciole::centerMotion(at("/a/b/c"), at("/a")) == CenterMotion::Up);
    TEST_CHECK(luciole::centerMotion(at("/a/b"), at("/a/c")) == CenterMotion::Across);
    TEST_CHECK(luciole::centerMotion(at("/a/b"), at("/a/bc")) == CenterMotion::Across);
  }

  void stripsStandForTheFoldersAbove() {
    Rig rig;
    rig.view->setFolder(at("/l1/l2/l3"));
    rig.layout();
    const auto& folders = rig.view->ancestors();
    TEST_CHECK(folders.size() == 3 && folders[0] == at("/l1/l2") && folders[2] == at("/"));
    TEST_CHECK(rig.view->strips().count() == 3);
    // another pane points at a folder: its strip is marked, farthest first
    palette.primaryContainer = rgba(0.20F, 0.30F, 0.60F, 1);
    palette.surfaceContainerHigh = rgba(0.25F, 0.25F, 0.25F, 1);
    const auto fill = [&rig](std::size_t index) { return stripFill(*rig.view->strips().area(index)); };
    rig.view->setPointedFolder(at("/l1"));
    TEST_CHECK(fill(1) == palette.primaryContainer && fill(2) != palette.primaryContainer);
    rig.view->setPointedFolder(at("/elsewhere"));
    TEST_CHECK(fill(1) != palette.primaryContainer);
    rig.view->setPointedFolder(at("/l1/l2"));
    rig.view->setPointedFolder(std::nullopt);
    TEST_CHECK(fill(2) != palette.primaryContainer);
    // a click asks for the strip's folder with its button, and moves no focus
    const auto* nearest = rig.view->strips().nearest();
    float x = 0;
    float y = 0;
    Node::absolutePosition(nearest, x, y);
    x += nearest->width() / 2;
    y += nearest->height() / 2;
    std::uint32_t time = 1000;
    for (const std::uint32_t button : {BTN_LEFT, BTN_MIDDLE}) {
      time += 1000; // far apart, so no double press
      rig.dispatcher.pointerMotion(x, y, ++rig.serial);
      rig.dispatcher.pointerButton(x, y, button, true, ++rig.serial, time, false);
      rig.dispatcher.pointerButton(x, y, button, false, ++rig.serial, time + 50, false);
    }
    TEST_CHECK(rig.asked.size() == 2 && rig.asked[0].first == at("/l1/l2") && rig.asked[0].second == BTN_LEFT);
    TEST_CHECK(rig.asked[1].second == BTN_MIDDLE);
    TEST_CHECK(!rig.view->takeStripFocus());
  }

  // After a strip's key changed the folder, the focus goes to the nearest strip shown, or to the list without one.
  void stripKeyLeavesFocusOnTheStrips() {
    Rig rig;
    rig.view->setFolder(at("/l1/l2/l3/l4"));
    rig.layout();
    rig.dispatcher.setFocus(rig.view->strips().nearest());
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(rig.asked.size() == 1 && rig.asked[0].first == at("/l1/l2/l3") && rig.asked[0].second == 0);
    TEST_CHECK(!rig.view->takeStripFocus()); // the folder has not changed yet
    rig.view->setFolder(at("/l1/l2/l3"));
    const auto focus = rig.view->takeStripFocus();
    TEST_CHECK(focus.has_value() && *focus == rig.view->strips().nearest() && *focus != nullptr);
    TEST_CHECK(rig.view->strips().count() == 3 && rig.view->strips().label(2)->text() == "l2");
    TEST_CHECK(!rig.view->takeStripFocus()); // asked once
    // the focus going elsewhere meanwhile drops the request
    rig.dispatcher.setFocus(rig.view->strips().nearest());
    rig.dispatcher.keyEvent(XKB_KEY_space, 0, 0, true);
    rig.dispatcher.setFocus(rig.list);
    rig.view->setFolder(at("/l1/l2"));
    TEST_CHECK(!rig.view->takeStripFocus());
    // no strip shown in the new folder: the list
    rig.view->setStripsShown(false);
    rig.layout();
    rig.view->setStripsShown(true);
    rig.layout();
    rig.dispatcher.setFocus(rig.view->strips().nearest());
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    rig.view->setStripsShown(false);
    rig.view->setFolder(at("/l1"));
    const auto none = rig.view->takeStripFocus();
    TEST_CHECK(none.has_value() && *none == nullptr);
  }

  void deeperSlidesInFromTheColumns() {
    Rig rig;
    rig.view->setFolder(at("/a/b"));
    rig.layout();
    TEST_CHECK(!rig.view->moving() && atRest(*rig.list));
    rig.view->setFolder(at("/a/b/c"));
    TEST_CHECK(rig.view->moving());
    TEST_NEAR(rig.list->translateX(), kSlide, 0.001F);
    TEST_NEAR(rig.list->opacity(), 0.35F, 0.001F);
    TEST_NEAR(rig.columns[0]->translateX(), kSlide, 0.001F);
    TEST_NEAR(rig.columns[0]->opacity(), 0.0F, 0.001F);
    TEST_NEAR(rig.view->strips().translateX(), kSlide, 0.001F);
    rig.layout(); // the frame of the change lays out once
    rig.settle();
    for (const Node* node :
         {static_cast<Node*>(rig.list), static_cast<Node*>(rig.columns[0]), static_cast<Node*>(rig.columns[2]),
          static_cast<Node*>(&rig.view->strips())}) {
      TEST_CHECK(atRest(*node));
    }
  }

  void upSlidesTheOtherWayAndMirrors() {
    Rig rig;
    rig.view->setFolder(at("/a/b/c"));
    rig.layout();
    rig.view->setFolder(at("/a"));
    TEST_NEAR(rig.list->translateX(), -kSlide, 0.001F);
    rig.layout();
    rig.settle();
    Style::setRtl(true);
    rig.layout();
    rig.view->setFolder(at("/a/b"));
    TEST_NEAR(rig.list->translateX(), -kSlide, 0.001F); // deeper, toward the columns on the left
    rig.layout();
    rig.settle();
    Style::setRtl(false);
    // across: a fade with no travel, shorter without strips
    rig.view->setStripsShown(false);
    rig.layout();
    rig.view->setFolder(at("/x"));
    TEST_NEAR(rig.list->translateX(), 0.0F, 0.001F);
    TEST_NEAR(rig.list->opacity(), 0.35F, 0.001F);
    rig.layout();
    rig.settle();
    TEST_CHECK(atRest(*rig.list));
  }

  void noMotionWhenTheToolkitSaysSo() {
    Rig rig;
    rig.view->setFolder(at("/a"));
    rig.view->setFolder(at("/a/b")); // never laid out
    TEST_CHECK(!rig.view->moving() && atRest(*rig.list));
    rig.layout();
    MotionService::instance().setEnabled(false);
    rig.view->setFolder(at("/a/b/c"));
    TEST_CHECK(!rig.view->moving() && !rig.animations.hasActive() && atRest(*rig.list) && atRest(*rig.columns[0]));
    MotionService::instance().setEnabled(true);
    rig.holder->setVisible(false); // the expanded instrument covers the center
    rig.view->setFolder(at("/a"));
    TEST_CHECK(!rig.view->moving() && atRest(*rig.list));
  }

  // A node handed back is at rest; a preview's slot keeps still; a view destroyed while moving stops.
  void handedBackAndPreviewNodesStayStill() {
    Rig rig;
    rig.view->setFolder(at("/a"));
    rig.view->setSpanFrom(1);
    rig.layout();
    rig.view->setFolder(at("/a/b"));
    TEST_CHECK(rig.columns[0]->opacity() == 0 && atRest(*rig.columns[1]) && atRest(*rig.columns[2]));
    rig.animations.tick(32);
    auto list = rig.view->setList(std::make_unique<Node>());
    TEST_CHECK(list.get() == rig.list && atRest(*list));
    auto column = rig.view->setColumn(0, std::make_unique<Node>());
    TEST_CHECK(column.get() == rig.columns[0] && atRest(*column));
    rig.animations.tick(16);
    TEST_CHECK(rig.view->moving() && rig.view->list()->opacity() < 1);
    rig.holder->removeChild(rig.view).reset();
    rig.animations.tick(16);
    TEST_CHECK(!rig.animations.hasActive());
  }

  // A preview that takes a slot as the motion starts or while it runs leaves the slots from it on at rest, so the
  // columns it covered show in their places once it goes.
  void previewDuringTheMotionLeavesTheColumnsAtRest() {
    Rig rig;
    rig.view->setFolder(at("/a"));
    rig.layout();
    rig.view->setFolder(at("/a/b")); // every column fades in from nothing
    rig.view->setSpanFrom(0);        // in the same frame
    rig.layout();
    rig.settle();
    rig.view->setSpanFrom(std::nullopt);
    rig.layout();
    for (const auto* column : rig.columns) {
      TEST_CHECK(column->visible() && atRest(*column));
    }
    rig.view->setFolder(at("/a/b/c"));
    rig.layout();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    rig.animations.tick(16);
    TEST_CHECK(rig.view->moving() && rig.columns[2]->opacity() < 1);
    rig.view->setSpanFrom(1);
    TEST_CHECK(atRest(*rig.columns[1]) && atRest(*rig.columns[2]));
    TEST_CHECK(rig.columns[0]->opacity() < 1); // the slots before the preview keep moving
    rig.layout();
    rig.settle();
    rig.view->setSpanFrom(std::nullopt);
    rig.layout();
    TEST_CHECK(rig.columns[2]->visible() && atRest(*rig.columns[2]) && atRest(*rig.columns[1]));
  }

  // Hidden in the middle of its motion, the center stops it once asked to settle; shown, it keeps moving.
  void hiddenCenterStopsItsMotion() {
    Rig rig;
    rig.view->setFolder(at("/a"));
    rig.layout();
    rig.view->setFolder(at("/a/b"));
    rig.layout();
    rig.view->settle();
    TEST_CHECK(rig.view->moving());
    rig.holder->setVisible(false);
    rig.view->settle();
    TEST_CHECK(!rig.view->moving() && !rig.animations.hasActive());
    TEST_CHECK(atRest(*rig.list) && atRest(*rig.columns[0]) && atRest(rig.view->strips()));
  }
} // namespace

int main() {
  directionsOfAChange();
  stripsStandForTheFoldersAbove();
  stripKeyLeavesFocusOnTheStrips();
  deeperSlidesInFromTheColumns();
  upSlidesTheOtherWayAndMirrors();
  noMotionWhenTheToolkitSaysSo();
  handedBackAndPreviewNodesStayStill();
  previewDuringTheMotionLeavesTheColumnsAtRest();
  hiddenCenterStopsItsMotion();
  std::println("workbench center folder checks passed");
}
