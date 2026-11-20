#pragma once

#include "workbench/atlas_hud.h"
#include "workbench/atlas_motion.h"
#include "workbench/color.h"
#include "workbench/flame_row.h"
#include "workbench/pane_layout.h"
#include "workbench/scene_shapes.h"
#include "workbench/tree.h"

#include <vector>

// The Atlas as one frame draws it: the map's scene seen through the current camera, an old picture fading out over
// it, and the chips and zoom buttons over both.
namespace luciole {
  // The transparent shape under everything that takes clicks on the map's empty space.
  workbench::ShapeId atlasBackgroundId();
  // The opaque bar over the map's top in a small view, under the chips and buttons; a click on it asks for nothing.
  workbench::ShapeId atlasBarId();

  // The chips and buttons for a view of `view`, laid out for the path of `tree` with text measured by `font`.
  struct AtlasHud {
    workbench::HudLayout layout;
    std::vector<workbench::Rect> placed;
  };
  AtlasHud atlasHud(const workbench::Tree& tree, workbench::ViewSize view, const workbench::LabelFont& font);

  // True when the view point (x, y) is on a chip, a button, or above `map`, where the bar is: not on the map.
  bool atlasControlAt(const AtlasHud& hud, const workbench::Rect& map, double x, double y);

  // `scene`, shapes in the map's coordinates of a scene built through another camera, moved by `frame` and then
  // down to the map's place in the view. Labels that start above the map are dropped.
  workbench::SceneShapes
  atlasMapLayer(workbench::SceneShapes scene, const workbench::Reframe& frame, const workbench::Rect& map);

  // `map`, an atlasMapLayer result, with the old picture `fading` over it at `fadeOpacity` on a surface-colored
  // backing over `area`, taking no hits; while it fades, the map's labels fade in. Itself the picture a new
  // cross-fade starts from, so a fade under way goes on fading in it.
  workbench::SceneShapes atlasPicture(
      workbench::SceneShapes map, const workbench::SceneShapes* fading, double fadeOpacity, const workbench::Rect& area,
      const workbench::ColorPalette& palette
  );

  struct AtlasLayers {
    workbench::SceneShapes map;           // atlasMapLayer's result
    const workbench::SceneShapes* fading; // an old map layer, or null
    double fadeOpacity = 0;               // its opacity
    workbench::SceneShapes hud;
  };

  // In drawing order: the background over the map, atlasPicture of the map and the old picture, the bar over the
  // view's top above a map that starts lower, and the chips and buttons. The chips' labels come first, so they are
  // always shown.
  workbench::SceneShapes
  composeAtlas(AtlasLayers layers, const workbench::Rect& map, const workbench::ColorPalette& palette);
} // namespace luciole
