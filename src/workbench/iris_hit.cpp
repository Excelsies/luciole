#include "workbench/iris_hit.h"

#include "workbench/scene_shapes.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace luciole::workbench::iris {
  namespace {
    constexpr double kRingSlack = 1.5;         // context rings are thin: they take hits this far outside
    constexpr std::uint16_t kPartMask = 0xFFU; // the part index in the low byte of ShapeId::role

    std::optional<std::size_t> sectorAt(const std::vector<Sector>& sectors, double degrees) {
      for (std::size_t i = 0; i < sectors.size(); ++i) {
        if (ring::arcHolds(sectors[i].arc, sectors.front().arc.a0, degrees)) {
          return i;
        }
      }
      return std::nullopt;
    }
  } // namespace

  std::optional<Hit> hitTest(const Scene& scene, ring::Point point) {
    for (const auto* column : {&scene.rows.right, &scene.rows.left}) {
      for (const auto& row : *column) {
        if (row.shown
            && point.x >= row.box.x
            && point.x <= row.box.x + row.box.width
            && std::abs(point.y - row.y) <= ring::kRowHeight / 2) {
          return Hit{.kind = HitKind::Row, .ring = 0, .sector = row.sector};
        }
      }
    }
    const auto& dial = scene.dial;
    const double dx = point.x - dial.center.x;
    const double dy = point.y - dial.center.y;
    const double radius = std::hypot(dx, dy);
    const double degrees = ring::wrapDegrees(std::atan2(dx, -dy) / ring::kRadian);
    if (radius <= dial.rHub) {
      return Hit{.kind = HitKind::Hub, .ring = 0, .sector = 0};
    }
    if (radius >= dial.rIn && radius <= dial.rOut) {
      const auto sector = sectorAt(scene.main.sectors, degrees);
      return sector ? std::optional(Hit{.kind = HitKind::Arc, .ring = 0, .sector = *sector}) : std::nullopt;
    }
    for (std::size_t k = 0; k < scene.rings.size(); ++k) {
      const auto& band = scene.rings[k].band;
      if (radius >= band.r0 - kRingSlack && radius <= band.r1 + kRingSlack) {
        const auto sector = sectorAt(scene.rings[k].sectors, degrees);
        return sector ? std::optional(Hit{.kind = HitKind::Context, .ring = k, .sector = *sector}) : std::nullopt;
      }
    }
    return std::nullopt;
  }

  ShapeId chipId(std::size_t index) { return {.node = kNoNode, .role = shapeRole(SceneRole::Chip, index)}; }

  NodeId folderOf(const Scene& scene, ShapeId id) {
    switch (sceneRole(id.role)) {
    case SceneRole::Chip: {
      const auto& chips = scene.hub.chips;
      const std::size_t index = id.role & kPartMask;
      return id == chipId(index) && index < chips.size() ? chips[index].node : kNoNode;
    }
    case SceneRole::Stack: {
      const bool held =
          std::ranges::any_of(scene.stack.rows, [&id](const StackRow& row) { return row.node == id.node; });
      return held ? id.node : kNoNode;
    }
    default:
      return kNoNode;
    }
  }
} // namespace luciole::workbench::iris
