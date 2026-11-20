#pragma once

#include "ui/workbench/scene_part.h"

#include <memory>

namespace luciole {
  // The flow diagram ribbon: the current folder, three levels of folders ahead and the ancestors as a stem, at most its
  // tallest height against the bottom edge.
  std::unique_ptr<ScenePart> makeFlowPart();
} // namespace luciole
