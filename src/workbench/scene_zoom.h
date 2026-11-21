#pragma once

#include "workbench/scene_shapes.h"

namespace luciole::workbench {
  // Scales a scene laid out in a smaller view up to the real one: every position, size, radius and line width of the
  // shapes, and the boxes, icons and trails of the labels. Angles, strokes, the softness of edges and text sizes
  // stay: the toolkit scales text itself. A factor that is not positive and finite changes nothing.
  void zoomScene(SceneShapes& scene, double factor);
} // namespace luciole::workbench
