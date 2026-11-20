#pragma once

#include "workbench/atlas_camera.h"
#include "workbench/pane_layout.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// The Atlas as circles in view pixels: the current folder, its ancestors' rims, its siblings and every visible
// circle inside them.
namespace luciole::workbench {
  // Children of a circle at least this large in pixels are labeled and clickable.
  inline constexpr double kAtlasKidsLabeledFrom = 110;

  enum class BubbleRole : std::uint8_t { Ancestor, Self, Sibling, SiblingChild, Bubble, Grandchild, More };

  // One circle in view pixels.
  struct SceneItem {
    NodeId node = kNoNode;   // kNoNode for "+N more"
    NodeId folder = kNoNode; // the folder a "+N more" bubble belongs to
    BubbleRole role = BubbleRole::Bubble;
    double x = 0;
    double y = 0;
    double r = 0;
    double fade = 1;
    int depth = 0;         // levels below the current folder; negative for ancestors
    std::size_t level = 0; // ancestors: 1 for the parent
    std::size_t restCount = 0;
    bool clickable = false;
    bool kids = false;        // its children are drawn
    bool labeledKids = false; // its children are labeled and clickable
    bool empty = false;       // a folder with nothing to show
    bool huge = false;        // an ancestor rim too large to draw: it covers the view
  };

  // Circles in painter order: ancestors farthest first, the current folder's siblings, the current folder,
  // and each circle's children right after it.
  struct AtlasScene {
    std::vector<SceneItem> items;
    ViewSize view;
    Camera camera;
  };

  // 0 below `lo`, 1 above `hi`, linear between.
  double fadeRamp(double value, double lo, double hi);

  // The scene for the current folder `cwd` seen through `camera`. Empty for an unknown folder, a view
  // without area or a camera that is not finite with a positive scale.
  AtlasScene buildScene(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view);

  // As buildScene, keeping the circles up to `margin` of the view's width and height beyond each edge, in the
  // coordinates of `view`, which stays the scene's view. A margin that is not finite or not positive reads as 0.
  AtlasScene
  buildSceneAround(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double margin);

  // Topmost clickable item under (x, y).
  std::optional<std::size_t> hitTest(const AtlasScene& scene, double x, double y);

  // The item showing `node`, or its nearest shown ancestor inside the current folder.
  std::optional<std::size_t> findFocus(const AtlasScene& scene, const Tree& tree, NodeId node);
} // namespace luciole::workbench
