#pragma once

#include "ui/workbench/scene_part.h"
#include "workbench/atlas_scene.h"
#include "workbench/tree.h"

#include <cstdint>

// What the Atlas's arrow keys, Home and End do with the cursor.
namespace luciole {
  // Arrows select the bubble of `scene` nearest the cursor in their direction, or the first one when the cursor is on
  // none; Home and End select the first and last bubble. Handled with no request when the scene has no bubble or none
  // lies that way; unhandled for other keys.
  PartReaction atlasCursorKey(const workbench::AtlasScene& scene, std::uint32_t sym, workbench::NodeId cursor);
} // namespace luciole
