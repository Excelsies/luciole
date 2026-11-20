#pragma once

#include "ui/workbench/scene_part.h"

#include <functional>
#include <memory>

namespace luciole {
  // Atlas: the current folder as packed circles seen through a camera. The wheel and the zoom keys and buttons ease the
  // zoom, a drag pans, navigation flies the camera or cross-fades, and chips over the map lead to the ancestors. Its
  // layouts finish on a worker and report through `ready` on the main context.
  std::unique_ptr<ScenePart> makeAtlasPart(std::function<void()> ready);
} // namespace luciole
