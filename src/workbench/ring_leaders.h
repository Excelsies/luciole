#pragma once

#include "workbench/ring_geometry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Leader lines from the arcs of a full dial to their label rows.
namespace luciole::workbench::ring {
  enum class Side : std::uint8_t { Right, Left };

  struct LeaderItem {
    double degrees = 0; // middle of the arc
    double y = 0;       // row center
    Side side = Side::Right;
  };

  enum class LeaderMode : std::uint8_t { Straight, Lanes };

  using Polyline = std::vector<Point>;

  struct LeaderRoutes {
    std::vector<Polyline> paths; // aligned with the items
    LeaderMode mode = LeaderMode::Straight;
  };

  // Arc edge, out to rBase, then straight to the row.
  std::vector<Polyline> routeStraight(const Dial& dial, std::span<const LeaderItem> items);

  // Leaders that run along lane circles around the dial so they never cross.
  std::vector<Polyline> routeLanes(const Dial& dial, std::span<const LeaderItem> items);

  // Straight leaders when they neither cross nor cut into the dial, otherwise lane leaders.
  LeaderRoutes routeLeaders(const Dial& dial, std::span<const LeaderItem> items);

  // Pairs of polylines that properly cross.
  std::size_t crossings(std::span<const Polyline> paths);
} // namespace luciole::workbench::ring
