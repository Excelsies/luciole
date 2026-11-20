#pragma once

#include "workbench/iris_scene.h"
#include "workbench/ring_geometry.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>

// What lies under a point of an Iris scene.
namespace luciole::workbench::iris {
  enum class HitKind : std::uint8_t { Hub, Row, Arc, Context };

  struct Hit {
    HitKind kind = HitKind::Arc;
    std::size_t ring = 0;   // index into Scene::rings for Context
    std::size_t sector = 0; // index into the ring's sectors; the main ring for Row and Arc
    bool operator==(const Hit&) const = default;
  };

  // Piece under a point: a shown row, the hub, a main ring arc, then a context ring with 1.5 px of slack.
  std::optional<Hit> hitTest(const Scene& scene, ring::Point point);

  // The shape id of the trail chip at `index` among the hub's chips. It holds no node, so the folder a chip offers
  // never counts as a piece drawn on the dial and the pointer over a chip points at no entry.
  ShapeId chipId(std::size_t index);

  // The folder a click on a trail chip or an ancestor row enters; kNoNode for other shapes and for chips and rows the
  // scene does not hold.
  NodeId folderOf(const Scene& scene, ShapeId id);
} // namespace luciole::workbench::iris
