#include "atlas_view_rig.h"
#include "cpu_budget.h"
#include "instrument_rig.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/atlas_camera.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_atlas_fixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <linux/input-event-codes.h>
#include <optional>
#include <print>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Atlas instrument through its view: camera, chips, zoom buttons, panning and flights.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  namespace atlas = luciole::test::atlas;
  using namespace luciole::test::atlas_view;

  std::string tooltipAt(Rig& rig, ShapePoint at) {
    rig.dispatcher.pointerMotion(at.x, at.y, 1);
    const auto tip = rig.view->shapeView().inputArea().tooltipContent();
    return std::holds_alternative<std::string>(tip) ? std::get<std::string>(tip) : std::string{};
  }

  // Processor time of the calling thread.
  double threadMs() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
  }

  // A pane that grows keeps the folder at the same share of the map, as a fresh start at that size would draw it.
  void aGrowingPaneRescalesTheCamera() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 340, 380);
    rig.show(inputs);
    // The small pane puts the map under a 26 px bar: 340 by 354, at rest 340 / 2 - 14 = 156 px.
    TEST_NEAR(radiusOf(rig.view->shapeView(), 0), 156.0F, 0.01F);
    rig.key(XKB_KEY_plus, '+');
    settle(rig, inputs);
    TEST_NEAR(radiusOf(rig.view->shapeView(), 0), 156.0F * 1.6F, 0.05F);
    rig.resize(520, 844);
    settle(rig, inputs);
    // At 520 by 844 the rest radius is 246 px; the zoom keeps its ratio to it.
    TEST_NEAR(radiusOf(rig.view->shapeView(), 0), 246.0F * 1.6F, 0.05F);
    rig.key(XKB_KEY_0, '0');
    settle(rig, inputs);
    Rig fresh(WorkbenchInstrument::Atlas, 520, 844);
    fresh.show(inputs);
    TEST_NEAR(radiusOf(rig.view->shapeView(), 0), radiusOf(fresh.view->shapeView(), 0), 0.01F);
    TEST_NEAR(centerOf(rig.view->shapeView(), 0).y, centerOf(fresh.view->shapeView(), 0).y, 0.01F);
  }

  // `next` arrives while the pane is resized: its layout starts at the old size and lands at the new one. The view
  // settles as a fresh one of the new size draws `next`.
  void resizedWhileArriving(Rig& rig, const PartInputs& next, NodeId node, float width, float height) {
    rig.frame(next);
    rig.resize(width, height);
    rig.frame(next);
    rig.show(next);
    settle(rig, next);
    Rig fresh(WorkbenchInstrument::Atlas, width, height);
    fresh.show(next);
    settle(fresh, next);
    const auto& view = rig.view->shapeView();
    TEST_NEAR(radiusOf(view, node), radiusOf(fresh.view->shapeView(), node), 0.01F);
    TEST_NEAR(centerOf(view, node).x, centerOf(fresh.view->shapeView(), node).x, 0.01F);
    TEST_NEAR(centerOf(view, node).y, centerOf(fresh.view->shapeView(), node).y, 0.01F);
  }

  // A resize while a layout of a new snapshot runs: before the first one, and arriving at the same folder, at one
  // nearby and at one far away.
  void aResizeDuringArrivalRescales() {
    Rig first(WorkbenchInstrument::Atlas, 340, 380);
    resizedWhileArriving(first, inputsFor(balancedTree(), 1), 0, 520, 844);
    const auto top = inputsFor(balancedTree(), 1);
    Rig same(WorkbenchInstrument::Atlas, 900, 600);
    same.show(top);
    resizedWhileArriving(same, inputsFor(balancedTree(), 2), 0, 1300, 1000);
    Rig near(WorkbenchInstrument::Atlas, 900, 600);
    near.show(top);
    resizedWhileArriving(near, inputsFor(enteredAt(4), 2), 4, 1300, 1000);
    auto tree = atlas::atlasTree();
    const auto from = inputsFor(tree, 1);
    tree.root = atlas::kSub;
    tree.ancestors = parentChain(tree, tree.root);
    Rig far(WorkbenchInstrument::Atlas, 900, 600);
    far.show(from);
    resizedWhileArriving(far, inputsFor(std::move(tree), 2), atlas::kSub, 1300, 1000);
  }

  // The wheel's zoom lands over several frames: the first frame takes 40% of it on a log scale.
  void theWheelEasesItsZoom() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const float rest = radiusOf(view, 0);
    const auto center = centerOf(view, 0);
    TEST_CHECK(rig.wheel(center, -10));
    TEST_NEAR(radiusOf(view, 0), rest, 1e-4F); // nothing moves before the next frame
    const auto frames = rig.frameRequests;
    rig.frame(inputs);
    TEST_CHECK(rig.frameRequests > frames); // and it keeps asking for frames
    const double first = std::log(radiusOf(view, 0) / rest);
    TEST_CHECK(first > 0);
    settle(rig, inputs);
    const double all = std::log(radiusOf(view, 0) / rest);
    TEST_NEAR(first / all, 0.4, 1e-3);
    TEST_NEAR(centerOf(view, 0).x, center.x, 0.05F);
    TEST_CHECK(!rig.view->shapeView().transitioning());
  }

  // A drag pans without selecting what it started on, keeps the hover clear, and lays nothing out.
  void aDragPans() {
    const auto inputs = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto start = centerOf(view, 0);
    const auto from = pointOf(view, 2);
    const auto builds = rig.view->sceneBuilds();
    press(rig, from);
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == 2);
    rig.dispatcher.pointerMotion(from.x + 10, from.y, 1);
    TEST_CHECK(rig.hovers.back() == kNoNode);
    // The layout of the scene with its margin lands; after it, frames of the drag change no layout.
    rig.frame(inputs);
    pump(50);
    rig.frame(inputs);
    rig.root.clearDirty();
    for (int step = 2; step <= 4; ++step) {
      rig.dispatcher.pointerMotion(from.x + 10.0F * static_cast<float>(step), from.y, 1);
      rig.view->update(inputs, false);
      TEST_CHECK(!rig.root.layoutDirty());
    }
    TEST_NEAR(centerOf(view, 0).x, start.x + 40, 0.01F);
    TEST_NEAR(centerOf(view, 0).y, start.y, 0.01F);
    TEST_CHECK(rig.hovers.back() == kNoNode);
    release(rig, {.x = from.x + 40, .y = from.y});
    TEST_CHECK(rig.requests.empty());
    settle(rig, inputs);
    TEST_NEAR(centerOf(view, 0).x, start.x + 40, 0.01F);
    TEST_CHECK(rig.view->sceneBuilds() == builds);
    // A plain click still selects.
    rig.click(pointOf(view, 2), BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1 && rig.requests[0].kind == PartRequestKind::Select);
  }

  // A point of the map where only the empty background takes the click.
  std::optional<ShapePoint> emptyPoint(const ShapeView& view) {
    for (float y = 30; y < view.height(); y += 8) {
      for (float x = 2; x < view.width(); x += 8) {
        const auto index = hitShape(view.shapes(), {x, y}, 0);
        if (index
            && view.shapes()[*index].id.node == kNoNode
            && sceneRole(view.shapes()[*index].id.role) == SceneRole::Hub) {
          return ShapePoint{.x = x, .y = y};
        }
      }
    }
    return std::nullopt;
  }

  // A click on empty space goes up one level, as one on the parent's rim does; at the top it does nothing.
  void emptySpaceGoesUp() {
    Rig top(WorkbenchInstrument::Atlas);
    top.show(inputsFor(balancedTree(), 1));
    const auto atTop = emptyPoint(top.view->shapeView());
    TEST_CHECK(atTop.has_value());
    top.click(*atTop, BTN_LEFT);
    TEST_CHECK(top.requests.empty());
    const auto inputs = inputsFor(enteredAt(1), 1);
    Rig inside(WorkbenchInstrument::Atlas, 900, 600);
    inside.show(inputs);
    inside.key(XKB_KEY_minus, '-');
    settle(inside, inputs);
    const auto& view = inside.view->shapeView();
    const auto point = emptyPoint(view);
    TEST_CHECK(point.has_value());
    inside.click(*point, BTN_LEFT);
    TEST_CHECK(inside.requests.size() == 1);
    TEST_CHECK(inside.requests[0].kind == PartRequestKind::Go && inside.requests[0].node == 0);
    // The parent's rim, just outside the current circle.
    const auto center = centerOf(view, 1);
    const ShapePoint rim{.x = center.x, .y = center.y - radiusOf(view, 1) - 4};
    const auto index = hitShape(view.shapes(), rim, 0);
    TEST_CHECK(index && view.shapes()[*index].id.node == 0);
    inside.click(rim, BTN_LEFT);
    TEST_CHECK(inside.requests.size() == 2);
    TEST_CHECK(inside.requests[1].kind == PartRequestKind::Go && inside.requests[1].node == 0);
  }

  // In a small pane the chips and buttons share the bar above the map; a chip goes to its folder and names its path.
  void chipsInTheBar() {
    const auto inputs = inputsFor(enteredAt(5), 1);
    Rig rig(WorkbenchInstrument::Atlas);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* outer = hittable(view, SceneRole::Chip, 0);
    const auto* inner = hittable(view, SceneRole::Chip, 1);
    TEST_CHECK(outer != nullptr && inner != nullptr);
    TEST_CHECK(outer->rect.y == 3 && outer->rect.height == 20 && inner->rect.y == 3);
    TEST_CHECK(outer->rect.x < inner->rect.x); // farthest first
    const auto* zoomIn = hittable(view, SceneRole::Button, kNoNode, 0);
    TEST_CHECK(zoomIn != nullptr && zoomIn->rect.y == 3 && zoomIn->rect.height == 20);
    TEST_CHECK(inner->rect.x + inner->rect.width <= zoomIn->rect.x);
    TEST_CHECK(!visibleLabels(view, "r").empty());
    TEST_CHECK(!visibleLabels(view, "a").empty());
    // The map starts under the bar: its center is 26 + 354 / 2 px down.
    TEST_NEAR(centerOf(view, 5).y, 203.0F, 0.01F);
    const ShapePoint chip{.x = inner->rect.x + inner->rect.width / 2, .y = inner->rect.y + 10};
    TEST_CHECK(tooltipAt(rig, chip) == "r/a");
    TEST_CHECK(rig.hovers.back() == 1);
    rig.click({.x = outer->rect.x + outer->rect.width / 2, .y = 13}, BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == 0);
  }

  // In a large pane the chips stack at the top left with the current folder last, and the buttons sit at the bottom
  // right: zoom in, zoom out and recenter, each with its tooltip.
  void buttonsZoomAndRecenter() {
    const auto inputs = inputsFor(enteredAt(1), 1);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const auto* parent = hittable(view, SceneRole::Chip, 0);
    TEST_CHECK(parent != nullptr && parent->rect.x == 12 && parent->rect.y == 12);
    const auto current = std::ranges::find_if(view.shapes(), [](const Shape& shape) {
      return shape.id.node == 1 && sceneRole(shape.id.role) == SceneRole::Chip;
    });
    TEST_CHECK(current != view.shapes().end() && !current->hittable && current->rect.y > parent->rect.y);
    const auto at = [&](std::size_t k) {
      const auto* button = hittable(view, SceneRole::Button, kNoNode, k);
      TEST_CHECK(button != nullptr && button->rect.width == 30);
      return ShapePoint{.x = button->rect.x + 15, .y = button->rect.y + 15};
    };
    TEST_CHECK(at(2).y > at(0).y && at(0).x == 900 - 12 - 15);
    TEST_CHECK(tooltipAt(rig, at(0)) == "Zoom in (+)");
    TEST_CHECK(tooltipAt(rig, at(1)) == "Zoom out (-)");
    TEST_CHECK(tooltipAt(rig, at(2)) == "Recenter (0)");
    const float rest = radiusOf(view, 1);
    rig.click(at(1), BTN_LEFT);
    settle(rig, inputs);
    TEST_NEAR(radiusOf(view, 1), rest / 1.6F, 0.01F);
    rig.click(at(0), BTN_LEFT);
    settle(rig, inputs);
    TEST_NEAR(radiusOf(view, 1), rest, 0.01F);
    rig.click(at(1), BTN_LEFT);
    rig.click(at(1), BTN_LEFT);
    settle(rig, inputs);
    rig.click(at(2), BTN_LEFT);
    settle(rig, inputs);
    TEST_NEAR(radiusOf(view, 1), rest, 0.01F);
    TEST_CHECK(rig.requests.empty());
  }

  // The surface-colored backings of the old pictures a cross-fade shows over a 900 px wide map.
  std::size_t fadeBackings(const ShapeView& view) {
    return static_cast<std::size_t>(std::ranges::count_if(view.shapes(), [](const Shape& shape) {
      return shape.id.node == kNoNode
          && sceneRole(shape.id.role) == SceneRole::Detail
          && shape.kind == ShapeKind::Rect
          && shape.rect.width == 900
          && shape.opacity > 0;
    }));
  }

  // Entering a folder nearby flies from where it was drawn to rest; one far away cross-fades at rest.
  void navigationFliesOrFades() {
    const auto top = inputsFor(balancedTree(), 1);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(top);
    const auto& view = rig.view->shapeView();
    const float rest = radiusOf(view, 0);
    const float before = radiusOf(view, 1);
    const auto near = inputsFor(enteredAt(1), 2);
    rig.show(near);
    TEST_CHECK(!view.transitioning());
    TEST_NEAR(radiusOf(view, 1), before, before * 0.01F);
    float last = radiusOf(view, 1);
    bool grew = true;
    rig.until(near, [&] {
      const float now = radiusOf(view, 1);
      grew = grew && now >= last - 1e-3F;
      last = now;
      return std::abs(now - rest) < 0.01F;
    });
    TEST_CHECK(grew);

    auto tree = atlas::atlasTree();
    const auto from = inputsFor(tree, 1);
    tree.root = atlas::kSub;
    tree.ancestors = parentChain(tree, tree.root);
    const auto far = inputsFor(std::move(tree), 2);
    Rig fading(WorkbenchInstrument::Atlas, 900, 600);
    fading.show(from);
    fading.show(far);
    TEST_CHECK(fadeBackings(fading.view->shapeView()) == 1);
    TEST_NEAR(radiusOf(fading.view->shapeView(), atlas::kSub), rest, 0.01F);
    fading.until(far, [&] { return fadeBackings(fading.view->shapeView()) == 0; });
  }

  // A far arrival while a cross-fade runs fades out the picture as shown, the older one still fading in it.
  void crossFadesStack() {
    auto tree = atlas::atlasTree();
    const auto top = inputsFor(tree, 1);
    tree.root = atlas::kSub;
    tree.ancestors = parentChain(tree, tree.root);
    const auto sub = inputsFor(std::move(tree), 2);
    const auto back = inputsFor(atlas::atlasTree(), 3);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(top);
    rig.show(sub);
    TEST_CHECK(fadeBackings(rig.view->shapeView()) == 1);
    rig.show(back);
    TEST_CHECK(fadeBackings(rig.view->shapeView()) == 2);
    rig.until(back, [&] { return fadeBackings(rig.view->shapeView()) == 0; });
  }

  // Root, 100 folders of 49 files: 5,000 entries.
  Tree fiveThousand() {
    Tree tree;
    tree.now = 6000;
    tree.nodes.push_back({.name = "r", .folder = true});
    constexpr std::size_t kFolders = 100;
    constexpr std::size_t kFiles = 49;
    tree.nodes[0].childCount = kFolders;
    for (std::size_t i = 0; i < kFolders; ++i) {
      tree.children.push_back(static_cast<NodeId>(1 + i));
      tree.nodes.push_back({
          .name = "d" + std::to_string(i),
          .parent = 0,
          .bytes = 4000.0 + static_cast<double>(i % 7) * 300,
          .files = kFiles,
          .items = kFiles,
          .folder = true,
      });
    }
    for (std::size_t i = 0; i < kFolders; ++i) {
      tree.nodes[1 + i].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[1 + i].childCount = kFiles;
      for (std::size_t j = 0; j < kFiles; ++j) {
        tree.children.push_back(static_cast<NodeId>(tree.nodes.size()));
        tree.nodes.push_back({
            .name = "f" + std::to_string(j) + ".txt",
            .parent = static_cast<NodeId>(1 + i),
            .bytes = 10.0 + static_cast<double>(j % 13),
        });
      }
    }
    tree.nodes[0].items = kFolders;
    tree.root = 0;
    return tree;
  }

  // Frames until `done`, letting worker results land; fails the test after `limit`.
  template <typename Done>
  void waitFor(Rig& rig, const PartInputs& inputs, Done done, std::chrono::milliseconds limit = kWaitLimit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    rig.frame(inputs);
    while (!done()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      pump(1);
      rig.frame(inputs);
    }
  }

  // One frame of a pan over 5,000 entries: the drag's motion and the view's update, in processor time.
  void aPanFrameIsCheap() {
    const auto inputs = inputsFor(fiveThousand(), 1);
    TEST_CHECK(inputs.snapshot.tree->nodes.size() == 5001);
    Rig rig(WorkbenchInstrument::Atlas, 900, 600);
    rig.show(inputs);
    const auto& view = rig.view->shapeView();
    const float rest = radiusOf(view, 0);
    rig.key(XKB_KEY_plus, '+');
    rig.key(XKB_KEY_plus, '+');
    // Both zoom steps land, then the layout at that zoom draws the files.
    waitFor(rig, inputs, [&] {
      return std::abs(radiusOf(view, 0) - rest * 1.6F * 1.6F) < 0.05F && view.shapes().size() > 1000;
    });
    settle(rig, inputs);
    TEST_NEAR(radiusOf(view, 0), rest * 1.6F * 1.6F, 0.05F);
    const std::size_t circles = view.shapes().size();
    const auto builds = rig.view->sceneBuilds();
    const ShapePoint from{.x = 300, .y = 300};
    press(rig, from);
    rig.dispatcher.pointerMotion(from.x + 8, from.y, 1);
    rig.frame(inputs);
    // The drag asks for the scene with its margin; the frames measured come after it.
    rig.until(inputs, [&] { return view.shapes().size() != circles; });
    constexpr int kFrames = 20;
    double total = 0;
    double most = 0;
    for (int frame = 1; frame <= kFrames; ++frame) {
      const double begin = threadMs();
      rig.dispatcher.pointerMotion(from.x + 8 + static_cast<float>(frame) * 2, from.y, 1);
      rig.view->update(inputs, false);
      rig.view->arrange(rig.renderer, rig.rect);
      const double spent = threadMs() - begin;
      total += spent;
      most = std::max(most, spent);
    }
    const std::size_t panned = view.shapes().size();
    release(rig, {.x = from.x + 48, .y = from.y});
    // What building the scene and its shapes again would cost each frame, for the same view.
    PackingCache packings;
    const AtlasSource source{.tree = *inputs.snapshot.tree, .settings = inputs.settings, .packings = packings};
    const ViewSize map{.width = 900, .height = 600};
    const Camera zoomed{.cx = 0, .cy = 0, .scale = restScale(map) * 1.6 * 1.6};
    TEST_CHECK(!buildScene(source, 0, zoomed, map).items.empty());
    const double rebuildStart = threadMs();
    const auto scene = buildScene(source, 0, zoomed, map);
    const auto shapes = atlasShapes(*inputs.snapshot.tree, inputs.settings, scene, {}, inputs.palette, LabelFont{});
    const double rebuild = threadMs() - rebuildStart;
    std::println(
        "atlas pan frame over 5,000 entries, {} shapes at rest, {} with the margin while panning: {:.3f} ms average, "
        "{:.3f} ms at most; one scene build {:.3f} ms",
        circles, panned, total / kFrames, most, rebuild
    );
    TEST_CHECK(circles > 1000 && shapes.shapes.size() > 1000);
    TEST_CHECK(rig.view->sceneBuilds() == builds);
    TEST_CHECK(TEST_WITHIN_BUDGET(total / kFrames, 16));
  }
} // namespace

int main() {
  aGrowingPaneRescalesTheCamera();
  aResizeDuringArrivalRescales();
  theWheelEasesItsZoom();
  aDragPans();
  emptySpaceGoesUp();
  chipsInTheBar();
  buttonsZoomAndRecenter();
  navigationFliesOrFades();
  crossFadesStack();
  aPanFrameIsCheap();
  return 0;
}
