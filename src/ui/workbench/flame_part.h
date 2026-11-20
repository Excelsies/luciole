#pragma once

#include "ui/workbench/scene_part.h"

#include <memory>

namespace luciole {
  // The flame graph ribbon: the strata ahead of the current folder and its ancestors, in a band against the bottom
  // edge.
  std::unique_ptr<ScenePart> makeFlamePart();
} // namespace luciole
