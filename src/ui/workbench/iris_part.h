#pragma once

#include "ui/workbench/scene_part.h"

#include <memory>

namespace luciole {
  // Iris: the current folder as a ring dial with labeled rows and its ancestors as outer rings.
  std::unique_ptr<ScenePart> makeIrisPart();
} // namespace luciole
