#include "workbench/ring_leaders.h"

#include "workbench/rounding.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luciole::workbench::ring {
  namespace {
    constexpr double kLanePitch = 2.25; // radial distance between leader lanes
    constexpr double kLaneStep = 1.5;   // degrees per point along a lane
    constexpr double kMaxLaneSteps = 240.0;

    // The same angle moved by whole turns to within half a turn of `reference`.
    double unwrapNear(double degrees, double reference) {
      if (!std::isfinite(degrees) || !std::isfinite(reference)) {
        return degrees;
      }
      if (degrees - reference > 180.0) {
        return degrees - 360.0 * std::ceil((degrees - reference - 180.0) / 360.0);
      }
      if (reference - degrees > 180.0) {
        return degrees + 360.0 * std::ceil((reference - degrees - 180.0) / 360.0);
      }
      return degrees;
    }

    // Whether `degrees` lies in lo..hi, read around the turn.
    bool withinTurn(double low, double high, double degrees) { return wrapDegrees(degrees - low) < high - low - 1e-9; }

    double edgeX(const Dial& dial, const LeaderItem& item) {
      return item.side == Side::Right ? dial.right.x0 - 6.0 : dial.left.x1 + 6.0;
    }

    struct LaneEnd {
      double dy = 0;
      double x = 0;
      double degrees = 0;
    };

    // Where a lane of radius `lane` reaches the row height of `item`, clamped to the lane.
    LaneEnd laneEnd(const Dial& dial, const LeaderItem& item, double lane) {
      LaneEnd end;
      end.dy = std::max(-lane + 1.0, std::min(lane - 1.0, item.y - dial.center.y));
      end.x = dial.center.x + (item.side == Side::Right ? 1.0 : -1.0) * std::sqrt(lane * lane - end.dy * end.dy);
      end.degrees = unwrapNear(wrapDegrees(std::atan2(end.x - dial.center.x, -end.dy) / kRadian), item.degrees);
      return end;
    }

    // Longest-path lane levels: a leader whose radial part sits in another's angular footprint goes inside that
    // lane, and one whose turn ends in another's lane span goes outside it. At most n + 1 relaxation passes.
    std::vector<std::size_t>
    laneLevels(std::span<const double> starts, std::span<const double> ends, std::span<const double> edges) {
      const auto count = starts.size();
      std::vector<std::pair<std::size_t, std::size_t>> below;
      for (std::size_t a = 0; a < count; ++a) {
        for (std::size_t b = 0; b < count; ++b) {
          if (a == b) {
            continue;
          }
          const double hullLow = std::min({starts[b], ends[b], edges[b]});
          const double hullHigh = std::max({starts[b], ends[b], edges[b]});
          if (withinTurn(hullLow, hullHigh, starts[a])) {
            below.emplace_back(a, b);
          }
          const double spanLow = ends[b] < starts[b] ? ends[b] : starts[b];
          const double spanHigh = ends[b] < starts[b] ? starts[b] : ends[b];
          if (withinTurn(spanLow, spanHigh, ends[a])) {
            below.emplace_back(b, a);
          }
        }
      }
      std::vector<std::size_t> levels(count, 0);
      bool changed = true;
      for (std::size_t pass = 0; changed && pass <= count; ++pass) {
        changed = false;
        for (const auto& [low, high] : below) {
          if (levels[high] < levels[low] + 1) {
            levels[high] = levels[low] + 1;
            changed = true;
          }
        }
      }
      return levels;
    }

    double distanceToCenter(const Dial& dial, Point from, Point to) {
      const double vx = to.x - from.x;
      const double vy = to.y - from.y;
      const double length = vx * vx + vy * vy;
      const double t = length != 0
          ? std::max(0.0, std::min(1.0, ((dial.center.x - from.x) * vx + (dial.center.y - from.y) * vy) / length))
          : 0.0;
      return std::hypot(from.x + t * vx - dial.center.x, from.y + t * vy - dial.center.y);
    }

    double orientation(Point a, Point b, Point c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

    // Proper crossing of pq and rs: the crossing lies more than 0.4 px from every end, and near-parallel
    // segments (two leaders sharing a lane circle) do not count.
    bool segmentsCross(Point p, Point q, Point r, Point s) {
      const double d1 = orientation(p, q, r);
      const double d2 = orientation(p, q, s);
      const double d3 = orientation(r, s, p);
      const double d4 = orientation(r, s, q);
      const bool straddles = d1 * d2 < 0 && d3 * d4 < 0;
      if (!straddles) {
        return false;
      }
      const double lengths = std::hypot(q.x - p.x, q.y - p.y) * std::hypot(s.x - r.x, s.y - r.y);
      const double sine =
          std::abs((q.x - p.x) * (s.y - r.y) - (q.y - p.y) * (s.x - r.x)) / (lengths != 0 ? lengths : 1.0);
      if (sine < 0.04) {
        return false;
      }
      const double t = d3 / (d3 - d4);
      const Point hit{.x = p.x + t * (q.x - p.x), .y = p.y + t * (q.y - p.y)};
      return std::ranges::all_of(std::array{p, q, r, s}, [hit](Point end) {
        return std::hypot(end.x - hit.x, end.y - hit.y) > 0.4;
      });
    }

    bool polylinesCross(const Polyline& first, const Polyline& second) {
      for (std::size_t i = 0; i + 1 < first.size(); ++i) {
        for (std::size_t j = 0; j + 1 < second.size(); ++j) {
          if (segmentsCross(first[i], first[i + 1], second[j], second[j + 1])) {
            return true;
          }
        }
      }
      return false;
    }
  } // namespace

  std::vector<Polyline> routeStraight(const Dial& dial, std::span<const LeaderItem> items) {
    std::vector<Polyline> paths;
    paths.reserve(items.size());
    for (const auto& item : items) {
      paths.push_back(
          {polar(dial.center, dial.rOut, item.degrees),
           polar(dial.center, dial.rBase, item.degrees),
           {.x = edgeX(dial, item), .y = item.y}}
      );
    }
    return paths;
  }

  std::vector<Polyline> routeLanes(const Dial& dial, std::span<const LeaderItem> items) {
    std::vector<double> starts;
    std::vector<double> ends;
    std::vector<double> edges;
    starts.reserve(items.size());
    ends.reserve(items.size());
    edges.reserve(items.size());
    for (const auto& item : items) {
      starts.push_back(item.degrees);
      ends.push_back(laneEnd(dial, item, dial.rBase).degrees);
      const double edge = std::atan2(edgeX(dial, item) - dial.center.x, dial.center.y - item.y) / kRadian;
      edges.push_back(unwrapNear(wrapDegrees(edge), item.degrees));
    }
    auto levels = laneLevels(starts, ends, edges);
    const auto cap = static_cast<std::size_t>(std::max(0.0, roundHalfUp(dial.lane / kLanePitch)));
    std::vector<Polyline> paths;
    paths.reserve(items.size());
    for (std::size_t k = 0; k < items.size(); ++k) {
      const auto& item = items[k];
      const double lane = dial.rBase + static_cast<double>(std::min(levels[k], cap)) * kLanePitch;
      const double turn = laneEnd(dial, item, lane).degrees;
      Polyline path{polar(dial.center, dial.rOut, item.degrees), polar(dial.center, lane, item.degrees)};
      const double sweep = std::ceil(std::abs(turn - item.degrees) / kLaneStep);
      const auto steps = static_cast<std::size_t>(std::clamp(std::isnan(sweep) ? 1.0 : sweep, 1.0, kMaxLaneSteps));
      for (std::size_t step = 1; step <= steps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        path.push_back(polar(dial.center, lane, item.degrees + (turn - item.degrees) * t));
      }
      path.push_back({.x = edgeX(dial, item), .y = item.y});
      paths.push_back(std::move(path));
    }
    return paths;
  }

  LeaderRoutes routeLeaders(const Dial& dial, std::span<const LeaderItem> items) {
    auto straight = routeStraight(dial, items);
    const bool clear = std::ranges::all_of(straight, [&dial](const Polyline& path) {
      return distanceToCenter(dial, path[1], path[2]) >= dial.rTotal + 1.0;
    });
    if (clear && crossings(straight) == 0) {
      return {.paths = std::move(straight), .mode = LeaderMode::Straight};
    }
    return {.paths = routeLanes(dial, items), .mode = LeaderMode::Lanes};
  }

  std::size_t crossings(std::span<const Polyline> paths) {
    std::size_t count = 0;
    for (std::size_t a = 0; a < paths.size(); ++a) {
      for (std::size_t b = a + 1; b < paths.size(); ++b) {
        count += polylinesCross(paths[a], paths[b]) ? 1 : 0;
      }
    }
    return count;
  }
} // namespace luciole::workbench::ring
