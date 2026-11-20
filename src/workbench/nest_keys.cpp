#include "workbench/nest_keys.h"

#include <cmath>
#include <limits>

namespace luciole::workbench {
  namespace {
    constexpr double kAcross = 2; // weight of the sideways offset against the distance ahead
    constexpr double kLeastAhead = 1;

    TreemapPoint centerOf(const TreemapRect& box) { return {.x = box.x + box.w / 2, .y = box.y + box.h / 2}; }
  } // namespace

  std::vector<std::size_t> nestStops(const NestScene& scene) {
    std::vector<std::size_t> stops;
    std::optional<std::size_t> more;
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      const auto kind = scene.pieces[i].kind;
      if (kind == NestPieceKind::Tile) {
        stops.push_back(i);
      } else if (kind == NestPieceKind::More) {
        more = i;
      }
    }
    if (more) {
      stops.push_back(*more);
    }
    return stops;
  }

  std::optional<std::size_t>
  nestNearest(std::span<const TreemapRect> boxes, std::optional<std::size_t> from, double dx, double dy) {
    if (boxes.empty()) {
      return std::nullopt;
    }
    if (!from || *from >= boxes.size()) {
      return 0;
    }
    const auto start = centerOf(boxes[*from]);
    std::optional<std::size_t> best;
    double bestCost = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      const auto to = centerOf(boxes[i]);
      const double ox = to.x - start.x;
      const double oy = to.y - start.y;
      const double ahead = ox * dx + oy * dy;
      // A NaN offset or direction fails this test, so it picks nothing.
      if (i == *from || !(ahead > kLeastAhead)) {
        continue;
      }
      const double cost = ahead + kAcross * (std::abs(ox * dy) + std::abs(oy * dx));
      if (cost < bestCost) {
        bestCost = cost;
        best = i;
      }
    }
    return best ? best : from;
  }
} // namespace luciole::workbench
