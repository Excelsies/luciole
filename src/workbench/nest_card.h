#pragma once

#include "workbench/scene_shapes.h"
#include "workbench/treemap.h"

// The Nest's peek card laid over its scene.
namespace luciole::workbench {
  // `scene` with `card`, whose box is `box`, over it: the card's shapes on top, its labels first so the label cap
  // keeps them. The card hides a label of the scene whose glyphs reach more than 1.5 px over the box, judged along
  // the line by the label's whole room and across it by the middle of its glyphs, turned with the label, its icon
  // alike; the other labels of that node go with it, so a tile never keeps a measure without its name.
  SceneShapes nestWithCard(SceneShapes scene, SceneShapes card, const TreemapRect& box);
} // namespace luciole::workbench
