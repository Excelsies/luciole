#pragma once

#include "ui/workbench/scene_part.h"

#include <memory>

namespace luciole {
  // The ladder ribbon: one bar of entries per ancestor level, nearest at the bottom.
  std::unique_ptr<ScenePart> makeLadderPart();
} // namespace luciole
