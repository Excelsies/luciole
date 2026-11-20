#pragma once

#include "ui/workbench/scene_part.h"

#include <memory>

namespace luciole {
  // Nest: the current folder as a treemap inside rings of its ancestors' entries.
  std::unique_ptr<ScenePart> makeNestPart();
} // namespace luciole
