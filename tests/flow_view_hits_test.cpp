#include "metrics_renderer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/text_node.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/ribbon_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The flow diagram answers the pointer on its bars, their labels and the ancestor pills; the bands between columns
// take no hits, so a label over the band into a child still stands for its own folder.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using luciole::test::workbenchTree;

  constexpr float kWidth = 830;
  constexpr float kHeight = 280;
  constexpr double kMiB = 1048576;
  // Ids: 0 top, 1 work, 2 projects, 3 archive, 4 media, 5 docs, 6 alpha, 7 beta, 8 video, 9 photos, 10 assets (in
  // alpha), 11 raw (in video).
  constexpr NodeId kProjects = 2;
  constexpr NodeId kMedia = 4;
  constexpr NodeId kAlpha = 6;
  constexpr NodeId kVideo = 8;

  PartInputs inputs() {
    auto tree = workbenchTree(
        {
            {.parent = -1, .name = "top", .folder = true, .bytes = 300 * kMiB, .items = 1},
            {.parent = 0, .name = "work", .folder = true, .bytes = 205 * kMiB, .items = 4},
            {.parent = 1, .name = "projects", .folder = true, .bytes = 85 * kMiB, .items = 2},
            {.parent = 1, .name = "archive", .folder = true, .bytes = 70 * kMiB},
            {.parent = 1, .name = "media", .folder = true, .bytes = 46 * kMiB, .items = 2},
            {.parent = 1, .name = "docs", .folder = true, .bytes = 4 * kMiB},
            {.parent = 2, .name = "alpha", .folder = true, .bytes = 76 * kMiB, .items = 1},
            {.parent = 2, .name = "beta", .folder = true, .bytes = 9 * kMiB},
            {.parent = 4, .name = "video", .folder = true, .bytes = 40 * kMiB, .items = 1},
            {.parent = 4, .name = "photos", .folder = true, .bytes = 6 * kMiB},
            {.parent = 6, .name = "assets", .folder = true, .bytes = 76 * kMiB},
            {.parent = 8, .name = "raw", .folder = true, .bytes = 40 * kMiB},
        },
        6000
    );
    tree.root = 1;
    tree.ancestors = parentChain(tree, 1);
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = 1}, .settings = {}, .palette = {}
    };
  }

  struct Rig {
    ::test::MetricsRenderer renderer;
    AnimationManager animations;
    ::Node root;
    RibbonView* view = nullptr;
    InputDispatcher dispatcher;
    std::vector<PartRequest> requests;
    std::vector<NodeId> hovers;
    LayoutRect rect{.x = 0, .y = 0, .width = kWidth, .height = kHeight};

    Rig() {
      root.setSize(2000, 2000);
      root.setAnimationManager(&animations);
      view = static_cast<RibbonView*>(root.addChild(std::make_unique<RibbonView>(WorkbenchRibbon::Flow, [] {
        return 0U;
      })));
      view->onRequest = [this](const PartRequest& request) { requests.push_back(request); };
      view->onHover = [this](NodeId node) { hovers.push_back(node); };
      dispatcher.setSceneRoot(&root);
      view->arrange(renderer, rect);
      for (int frame = 0; frame < 2; ++frame) {
        view->update(inputs(), false);
        view->arrange(renderer, rect);
      }
    }

    void move(ShapePoint at) { dispatcher.pointerMotion(at.x, at.y, 1); }

    void click(ShapePoint at) {
      move(at);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, true, 2, 0, false);
      dispatcher.pointerButton(at.x, at.y, BTN_LEFT, false, 3, 0, false);
    }
  };

  // A point on the text of the visible label `text`, past the end of its bar's hit area. The test renderer draws a
  // 12 px label 12 px tall above its baseline.
  std::optional<ShapePoint> onLabel(const ShapeView& view, const std::string& text) {
    for (const auto& child : view.labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label != nullptr && label->visible() && label->text() == text) {
        return ShapePoint{.x = label->x() + 6, .y = label->y() - 6};
      }
    }
    return std::nullopt;
  }

  bool inBand(const ShapeView& view, NodeId child, ShapePoint at) {
    return std::ranges::any_of(view.shapes(), [&](const auto& shape) {
      return shape.kind == ShapeKind::Ribbon && shape.id.node == child && shapeContains(shape, at, 0);
    });
  }

  // Resting on a folder's label marks that folder, and a click on it enters that folder, though the band into its
  // largest child lies under the label.
  void aLabelOverABandStandsForItsFolder() {
    for (const auto& [folder, child, name] :
         {std::tuple{kMedia, kVideo, "media"}, std::tuple{kProjects, kAlpha, "projects"}}) {
      Rig rig;
      const auto& view = rig.view->shapeView();
      const auto at = onLabel(view, name);
      TEST_CHECK(at.has_value());
      if (!at) {
        continue;
      }
      TEST_CHECK(inBand(view, child, *at));
      rig.move(*at);
      TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == folder);
      rig.click(*at);
      TEST_CHECK(rig.requests.size() == 1);
      TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == folder);
    }
  }

  // Away from bars, labels and pills, a band is no target: resting there marks nothing and a click asks for nothing.
  void aBandAloneTakesNoHits() {
    Rig rig;
    const auto& view = rig.view->shapeView();
    std::optional<ShapePoint> open;
    for (const auto& shape : view.shapes()) {
      if (shape.kind != ShapeKind::Ribbon || shape.id.node != kVideo) {
        continue;
      }
      // The middle of the band, halfway between the media bar and the video bar.
      const auto& top = shape.ribbon.top;
      const auto& bottom = shape.ribbon.bottom;
      const ShapePoint middle{
          .x = (top[0].x + top[3].x) / 2, .y = (top[1].y + top[2].y + bottom[1].y + bottom[2].y) / 4
      };
      if (shapeContains(shape, middle, 0) && !hitShape(view.shapes(), middle, 0).has_value()) {
        open = middle;
      }
    }
    TEST_CHECK(open.has_value());
    if (!open) {
      return;
    }
    rig.move(*open);
    TEST_CHECK(rig.hovers.empty() || rig.hovers.back() == kNoNode);
    rig.click(*open);
    TEST_CHECK(rig.requests.empty());
  }
} // namespace

int main() {
  aLabelOverABandStandsForItsFolder();
  aBandAloneTakesNoHits();
  return 0;
}
