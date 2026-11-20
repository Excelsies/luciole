#include "atlas_view_rig.h"
#include "core/timer_manager.h"
#include "instrument_rig.h"
#include "metrics_renderer.h"
#include "nest_rig.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/controls/tooltip_presenter.h"
#include "ui/workbench/ribbon_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// Hovering a piece of the Flame, the Flow, the Ladder, the Atlas or a merged piece of the Nest or the Flow gives its
// tooltip: the facts of the entry, and the reason when its amounts read "at least". Everything goes through the real
// part views and the input dispatcher.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  namespace fixture = luciole::test;
  using luciole::test::instrument::inside;
  using luciole::test::instrument::shapeOf;

  constexpr float kWidth = 700;
  constexpr float kHeight = 280;
  constexpr double kMiB = 1048576;

  constexpr NodeId kTop = 0;
  constexpr NodeId kUsr = 1;
  constexpr NodeId kHome = 2;
  constexpr NodeId kLib = 4;

  // top holds usr (one unreadable folder inside, so a lower bound) and home (exact). Ids: 0 top, 1 usr, 2 home,
  // 3 secret and 4 lib (in usr), 5 me (in home).
  Tree systemTree(NodeId root) {
    auto tree = fixture::workbenchTree(
        {{.parent = -1, .name = "top", .folder = true, .bytes = 300 * kMiB, .items = 2, .files = 4},
         {.parent = 0, .name = "usr", .folder = true, .bytes = 200 * kMiB, .items = 2, .files = 3},
         {.parent = 0, .name = "home", .folder = true, .bytes = 100 * kMiB, .items = 1, .files = 1},
         {.parent = 1, .name = "secret", .folder = true, .unreadable = true, .truncated = true},
         {.parent = 1, .name = "lib", .folder = true, .bytes = 200 * kMiB, .items = 1, .files = 3},
         {.parent = 2, .name = "me", .folder = true, .bytes = 100 * kMiB}},
        7200
    );
    for (const NodeId id : {NodeId{0}, NodeId{1}, NodeId{3}}) {
      tree.nodes[id].lowerBound = true;
    }
    tree.root = root;
    tree.ancestors = parentChain(tree, root);
    return tree;
  }

  PartInputs inputsOf(Tree tree) {
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = 1}, .settings = {}, .palette = {}
    };
  }

  std::string textOf(const TooltipContent& content) {
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  struct RibbonRig {
    ::test::MetricsRenderer renderer;
    AnimationManager animations;
    ::Node root;
    RibbonView* view = nullptr;
    InputDispatcher dispatcher;
    LayoutRect rect{.x = 0, .y = 0, .width = kWidth, .height = kHeight};

    explicit RibbonRig(WorkbenchRibbon kind) {
      root.setSize(2000, 2000);
      root.setAnimationManager(&animations);
      view = static_cast<RibbonView*>(root.addChild(std::make_unique<RibbonView>(kind, [] { return 0U; })));
      dispatcher.setSceneRoot(&root);
      view->arrange(renderer, rect);
    }

    void show(const PartInputs& inputs) {
      for (int turn = 0; turn < 3 && view->shapeView().shapes().empty(); ++turn) {
        view->update(inputs, false);
        view->arrange(renderer, rect);
      }
      view->update(inputs, true);
      view->arrange(renderer, rect);
      TEST_CHECK(!view->shapeView().shapes().empty());
    }

    // The tooltip with the pointer moved to `at`.
    std::string tipAt(ShapePoint at) {
      dispatcher.pointerMotion(at.x, at.y, 1);
      return textOf(view->shapeView().inputArea().tooltipContent());
    }
  };

  // A point no hittable shape covers.
  ShapePoint emptyPoint(const ShapeView& view) {
    for (float y = 2; y < view.height(); y += 5) {
      for (float x = 2; x < view.width(); x += 5) {
        if (!hitShape(view.shapes(), {x, y}, 0)) {
          return {.x = x, .y = y};
        }
      }
    }
    TEST_CHECK(false);
    return {};
  }

  void everyRibbonPieceTellsItsFacts() {
    for (const auto kind : {WorkbenchRibbon::Flame, WorkbenchRibbon::Flow, WorkbenchRibbon::Ladder}) {
      // The ladder draws the entries of the ancestors of the current folder, the others the folders ahead of it.
      const auto inputs = inputsOf(systemTree(kind == WorkbenchRibbon::Ladder ? kLib : kTop));
      RibbonRig rig(kind);
      rig.show(inputs);
      const auto& view = rig.view->shapeView();
      const auto* usr = shapeOf(view, kUsr);
      const auto* home = shapeOf(view, kHome);
      TEST_CHECK(usr != nullptr && home != nullptr);
      if (usr == nullptr || home == nullptr) {
        return;
      }
      const auto lower = rig.tipAt(inside(*usr));
      TEST_CHECK(lower.contains("at least 2 items") && lower.contains("modified 2 h ago"));
      if (kind == WorkbenchRibbon::Ladder) {
        // usr is above the open folder there: no share, and the sentence of a folder above it.
        TEST_CHECK(lower.starts_with("usr\nat least 200.0\u00A0MiB  ·  at least 2 items"));
        TEST_CHECK(lower.ends_with(
            "\nNot counted: measuring this folder stopped at a limit, at another filesystem or at a folder that cannot "
            "be "
            "read."
        ));
      } else {
        TEST_CHECK(lower.starts_with("usr\nat least 200.0\u00A0MiB  ·  about "));
        TEST_CHECK(lower.ends_with("\nNot counted: 1 unreadable folder (secret)."));
      }
      const auto exact = rig.tipAt(inside(*home));
      TEST_CHECK(exact.starts_with("home\n100.0 MiB  ·  about "));
      TEST_CHECK(exact.contains("1 item") && !exact.contains("Not counted"));
      // Over empty space there is none, and leaving the view takes it away.
      TEST_CHECK(rig.tipAt(emptyPoint(view)).empty());
      rig.tipAt(inside(*usr));
      rig.dispatcher.pointerLeave();
      TEST_CHECK(std::holds_alternative<std::monostate>(view.inputArea().tooltipContent()));
    }
  }

  // The current folder's own bar tells nothing its header does not, unless its total reads "at least": then it says
  // why, wherever the total is printed.
  void theCurrentFolderSaysWhyOnlyForALowerBound() {
    for (const auto kind : {WorkbenchRibbon::Flame, WorkbenchRibbon::Flow}) {
      for (const NodeId current : {kHome, kUsr}) {
        RibbonRig rig(kind);
        rig.show(inputsOf(systemTree(current)));
        std::size_t hovered = 0;
        for (const auto& shape : rig.view->shapeView().shapes()) {
          if (shape.hittable && shape.id.node == current) {
            const auto tip = rig.tipAt(inside(shape));
            ++hovered;
            TEST_CHECK(current == kHome ? tip.empty() : tip.ends_with("\nNot counted: 1 unreadable folder (secret)."));
          }
        }
        TEST_CHECK(hovered > 0);
      }
    }
  }

  // More than eight folders fold the lightest into a More record, which says why its total reads "at least".
  void aFoldedGroupSaysWhyItsTotalIsALowerBound() {
    std::vector<fixture::WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 400 * kMiB, .items = 12}
    };
    std::vector<std::string> names;
    names.reserve(12);
    for (int i = 0; i < 12; ++i) {
      names.push_back(std::format("g{:02}", i));
    }
    for (int i = 0; i < 12; ++i) {
      rows.push_back(
          {.parent = 0,
           .name = names[static_cast<std::size_t>(i)],
           .folder = true,
           .bytes = (40.0 - 2.0 * i) * kMiB,
           .items = 1,
           .truncated = true,
           .otherFilesystem = true}
      );
    }
    auto tree = fixture::workbenchTree(rows, 7200);
    for (auto& node : tree.nodes) {
      node.lowerBound = true;
    }
    const auto inputs = inputsOf(std::move(tree));
    RibbonRig rig(WorkbenchRibbon::Flow);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* more = shapeOf(view, kTop, SceneRole::More);
    TEST_CHECK(more != nullptr);
    if (more == nullptr) {
      return;
    }
    TEST_CHECK(
        rig.tipAt(inside(*more))
        == "4 more entries\nat least 84.0 MiB\nNot counted: g08, g09, g10 and 1 more are on another filesystem."
    );
  }

  Tree tooltipAtlasTree() {
    auto tree = luciole::test::instrument::balancedTree();
    // a (1) holds x (5), which could not be read.
    tree.nodes[0].lowerBound = true;
    tree.nodes[1].lowerBound = true;
    tree.nodes[5].unreadable = true;
    tree.nodes[5].truncated = true;
    tree.nodes[5].lowerBound = true;
    return tree;
  }

  void atlasCirclesTellTheirFacts() {
    using luciole::test::instrument::Rig;
    const auto inputs = inputsOf(tooltipAtlasTree());
    Rig rig(WorkbenchInstrument::Atlas, 520, 560);
    rig.show(inputs);
    luciole::test::atlas_view::settle(rig, inputs);
    const auto& view = rig.view->shapeView();
    const auto tip = [&](NodeId node) {
      const auto* shape = shapeOf(view, node);
      TEST_CHECK(shape != nullptr);
      const auto at = shape != nullptr ? inside(*shape) : ShapePoint{};
      rig.dispatcher.pointerMotion(at.x, at.y, 1);
      return textOf(view.inputArea().tooltipContent());
    };
    const auto lower = tip(1);
    TEST_CHECK(lower.starts_with("a\nat least 400 bytes"));
    TEST_CHECK(lower.ends_with("\nNot counted: 1 unreadable folder (x)."));
    const auto exact = tip(4);
    TEST_CHECK(exact.starts_with("c\n300 bytes") && !exact.contains("Not counted"));
    rig.dispatcher.pointerLeave();
    TEST_CHECK(std::holds_alternative<std::monostate>(view.inputArea().tooltipContent()));
    // The camera moving takes the piece's tooltip away until it rests: the circle under the pointer is changing.
    const auto at = inside(*shapeOf(view, 1));
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    TEST_CHECK(!textOf(view.inputArea().tooltipContent()).empty());
    rig.key(XKB_KEY_plus, '+');
    rig.frame(inputs);
    TEST_CHECK(textOf(view.inputArea().tooltipContent()).empty());
    luciole::test::atlas_view::settle(rig, inputs);
    TEST_CHECK(!textOf(view.inputArea().tooltipContent()).empty());
  }

  // Through the presenter, as the page shows tooltips: parked on a circle it appears after its delay, goes while the
  // camera moves and comes back, delay included, once it rests, with nothing but the layout passes asking.
  void atlasTooltipLeavesWithTheCameraAndComesBack() {
    using luciole::test::instrument::Rig;
    const auto inputs = inputsOf(tooltipAtlasTree());
    Rig rig(WorkbenchInstrument::Atlas, 520, 560);
    rig.show(inputs);
    luciole::test::atlas_view::settle(rig, inputs);
    const auto at = inside(*shapeOf(rig.view->shapeView(), 1));
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    TooltipPresenter presenter([] {});
    presenter.resume();
    const auto sync = [&] {
      TimerManager::instance().tick();
      presenter.sync(rig.renderer, rig.root, at.x, at.y, {rig.root.width(), rig.root.height()});
    };
    const auto shows = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!presenter.visible() && std::chrono::steady_clock::now() < deadline) { // bounded by the deadline
        sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return presenter.visible();
    };
    TEST_CHECK(shows());
    rig.key(XKB_KEY_plus, '+');
    rig.frame(inputs);
    sync();
    TEST_CHECK(!presenter.visible());
    luciole::test::atlas_view::settle(rig, inputs);
    TEST_CHECK(shows());
    presenter.dismiss();
  }

  // The Nest's "+N more" tile is the one merged piece that prints a size.
  void theNestsMoreTileSaysWhy() {
    std::vector<fixture::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .items = 300}};
    std::vector<std::string> names;
    names.reserve(300);
    for (int i = 0; i < 300; ++i) {
      names.push_back(std::format("d{:03}", i));
    }
    for (int i = 0; i < 300; ++i) {
      rows.push_back(
          {.parent = 0,
           .name = names[static_cast<std::size_t>(i)],
           .folder = true,
           .bytes = 1000.0 + (i % 17) * 50,
           .truncated = true,
           .otherFilesystem = true}
      );
    }
    auto tree = fixture::workbenchTree(rows, 6000);
    for (auto& node : tree.nodes) {
      node.lowerBound = true;
    }
    const auto inputs = inputsOf(std::move(tree));
    luciole::test::instrument::Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* more = shapeOf(view, 0, SceneRole::More);
    TEST_CHECK(more != nullptr);
    if (more == nullptr) {
      return;
    }
    const auto at = inside(*more);
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    const auto text = textOf(view.inputArea().tooltipContent());
    TEST_CHECK(text.contains(" more entries\nat least ") && text.contains("\nNot counted: d"));
    TEST_CHECK(text.ends_with(" more are on another filesystem."));
    // A merged piece whose total is exact has none.
    auto exact = fixture::workbenchTree(rows, 6000);
    const auto exactInputs = inputsOf(std::move(exact));
    luciole::test::instrument::Rig other(WorkbenchInstrument::Nest);
    other.show(exactInputs);
    const auto* exactMore = shapeOf(other.view->shapeView(), 0, SceneRole::More);
    TEST_CHECK(exactMore != nullptr);
    if (exactMore != nullptr) {
      const auto point = inside(*exactMore);
      other.dispatcher.pointerMotion(point.x, point.y, 1);
      TEST_CHECK(textOf(other.view->shapeView().inputArea().tooltipContent()).empty());
    }
  }

  // The card a folder opens has its own "+N more" tile, which says why its total reads "at least" too.
  void aCardsMoreTileSaysWhy() {
    std::vector<fixture::WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .items = 3},
        {.parent = 0, .name = "a", .folder = true, .bytes = 90000, .items = 80},
        {.parent = 0, .name = "b", .folder = true, .bytes = 5000, .items = 1},
        {.parent = 0, .name = "c.txt", .bytes = 4000},
    };
    std::vector<std::string> names;
    names.reserve(80);
    for (int i = 0; i < 80; ++i) {
      names.push_back(std::format("m{:02}", i));
    }
    for (int i = 0; i < 80; ++i) {
      rows.push_back(
          {.parent = 1,
           .name = names[static_cast<std::size_t>(i)],
           .folder = true,
           .bytes = 1000,
           .truncated = true,
           .otherFilesystem = true}
      );
    }
    auto tree = fixture::workbenchTree(rows, 6000);
    for (auto& node : tree.nodes) {
      node.lowerBound = true;
    }
    const auto inputs = inputsOf(std::move(tree));
    luciole::test::instrument::Rig rig(WorkbenchInstrument::Nest);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    rig.view->setCursor(1);
    rig.frame(inputs);
    rig.key(XKB_KEY_space, ' ');
    luciole::test::nest::tickUntil(rig, inputs, [&] { return luciole::test::nest::cardOf(view) != nullptr; });
    rig.finishTransition();
    rig.frame(inputs);
    const auto* more = shapeOf(view, 1, SceneRole::More);
    TEST_CHECK(more != nullptr);
    if (more == nullptr) {
      return;
    }
    const auto at = inside(*more);
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    const auto text = textOf(view.inputArea().tooltipContent());
    TEST_CHECK(text.contains(" more entries\nat least ") && text.ends_with(" more are on another filesystem."));
  }
} // namespace

int main() {
  everyRibbonPieceTellsItsFacts();
  theCurrentFolderSaysWhyOnlyForALowerBound();
  aFoldedGroupSaysWhyItsTotalIsALowerBound();
  atlasCirclesTellTheirFacts();
  atlasTooltipLeavesWithTheCameraAndComesBack();
  theNestsMoreTileSaysWhy();
  aCardsMoreTileSaysWhy();
  return 0;
}
