#include "ui/workbench/atlas_keys.h"

#include "core/input/key_symbols.h"
#include "workbench/atlas_camera.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace luciole {
  PartReaction atlasCursorKey(const workbench::AtlasScene& scene, std::uint32_t sym, workbench::NodeId cursor) {
    const auto step = arrowStep(sym);
    const bool edge = KeySymbol::isHome(sym) || KeySymbol::isEnd(sym);
    if (!step && !edge) {
      return {};
    }
    std::vector<workbench::NodeId> nodes;
    std::vector<workbench::Point> points;
    for (const auto& item : scene.items) {
      if (item.role == workbench::BubbleRole::Bubble && item.node != workbench::kNoNode) {
        nodes.push_back(item.node);
        points.push_back({.x = item.x, .y = item.y});
      }
    }
    PartReaction reaction{.handled = true, .relayout = false, .requests = {}};
    if (nodes.empty()) {
      return reaction;
    }
    std::optional<std::size_t> next;
    const auto current = std::ranges::find(nodes, cursor);
    if (edge) {
      next = KeySymbol::isHome(sym) ? 0 : nodes.size() - 1;
    } else if (current == nodes.end()) {
      next = 0;
    } else {
      next = workbench::nearest(points, static_cast<std::size_t>(current - nodes.begin()), step->dx, step->dy);
    }
    if (next) {
      reaction.requests.push_back({.kind = PartRequestKind::Select, .node = nodes[*next]});
    }
    return reaction;
  }
} // namespace luciole
