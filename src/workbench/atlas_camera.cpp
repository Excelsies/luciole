#include "workbench/atlas_camera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr double kAlongEpsilon = 1e-9;
    constexpr double kConeWidth = 1.8;
    constexpr double kPerpendicularCost = 2.2;
    constexpr double kFlatZoom = 1e-4;
    constexpr std::size_t kNearSteps = 2;

    // Both paths, one after the other; shared parents appear twice, which packings ignore.
    std::vector<NodeId> joinedPaths(const Tree& tree, NodeId first, NodeId second) {
      auto path = pathFromTop(tree, first);
      path.append_range(pathFromTop(tree, second));
      return path;
    }
  } // namespace

  bool usableCamera(const Camera& camera) {
    return std::isfinite(camera.cx) && std::isfinite(camera.cy) && std::isfinite(camera.scale) && camera.scale > 0;
  }

  std::optional<PackedItem>
  placementIn(const AtlasSource& source, NodeId parent, NodeId child, std::span<const NodeId> force) {
    if (!hasNode(source.tree, parent)) {
      return std::nullopt;
    }
    return source.packings.packing(source.tree, parent, source.settings, force)->placement(child);
  }

  Camera zoomAbout(const Camera& camera, double factor, double mx, double my) {
    const double scale = camera.scale * factor;
    if (!usableCamera(camera) || !std::isfinite(factor) || factor <= 0 || !std::isfinite(scale) || scale <= 0) {
      return camera;
    }
    const double ox = std::isfinite(mx) ? mx : 0.0;
    const double oy = std::isfinite(my) ? my : 0.0;
    const double qx = camera.cx + ox / camera.scale;
    const double qy = camera.cy + oy / camera.scale;
    return {.cx = qx - ox / scale, .cy = qy - oy / scale, .scale = scale};
  }

  Camera convert(const Camera& camera, const Similarity& sim) {
    return {.cx = sim.ax + sim.k * camera.cx, .cy = sim.ay + sim.k * camera.cy, .scale = camera.scale / sim.k};
  }

  Camera interpolate(const Camera& from, const Camera& to, double u) {
    if (std::isnan(u) || u <= 0) {
      return from;
    }
    if (u >= 1 || !usableCamera(from) || !usableCamera(to)) {
      return to;
    }
    const double scale = from.scale * std::pow(to.scale / from.scale, u);
    const double span = 1 / to.scale - 1 / from.scale;
    const double f = std::abs(std::log(to.scale / from.scale)) < kFlatZoom ? u : (1 / scale - 1 / from.scale) / span;
    return {.cx = from.cx + (to.cx - from.cx) * f, .cy = from.cy + (to.cy - from.cy) * f, .scale = scale};
  }

  std::optional<std::size_t> nearest(std::span<const Point> points, std::size_t current, double dx, double dy) {
    if (current >= points.size()) {
      return std::nullopt;
    }
    std::optional<std::size_t> best;
    std::optional<std::size_t> closest;
    double bestScore = std::numeric_limits<double>::infinity();
    double closestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (i == current) {
        continue;
      }
      const double vx = points[i].x - points[current].x;
      const double vy = points[i].y - points[current].y;
      const double along = vx * dx + vy * dy;
      if (along <= kAlongEpsilon) {
        continue;
      }
      const double perpendicular = std::abs(vx * dy - vy * dx);
      const double score = along + kPerpendicularCost * perpendicular;
      if (perpendicular <= along * kConeWidth && score < bestScore) {
        bestScore = score;
        best = i;
      }
      if (std::hypot(vx, vy) < closestDistance) {
        closestDistance = std::hypot(vx, vy);
        closest = i;
      }
    }
    return best ? best : closest;
  }

  std::size_t treeDistance(const Tree& tree, NodeId from, NodeId to) {
    if (!hasNode(tree, from) || !hasNode(tree, to)) {
      return 0;
    }
    auto x = from;
    auto y = to;
    auto depthX = depth(tree, x);
    auto depthY = depth(tree, y);
    std::size_t steps = 0;
    for (; depthX > depthY; --depthX, ++steps) {
      x = parentOf(tree, x);
    }
    for (; depthY > depthX; --depthY, ++steps) {
      y = parentOf(tree, y);
    }
    // Both climb together; a parent chain is at most nodes.size() long.
    for (std::size_t guard = 0; x != y && guard < tree.nodes.size(); ++guard, steps += 2) {
      x = parentOf(tree, x);
      y = parentOf(tree, y);
    }
    return steps;
  }

  std::optional<Similarity> similarity(const AtlasSource& source, NodeId from, NodeId to) {
    const auto& tree = source.tree;
    if (!hasNode(tree, from) || !hasNode(tree, to)) {
      return std::nullopt;
    }
    const auto force = joinedPaths(tree, from, to);
    Similarity sim;
    auto up = from;
    auto down = to;
    auto depthUp = depth(tree, up);
    auto depthDown = depth(tree, down);
    std::vector<NodeId> downs;
    const auto climb = [&] {
      const auto p = placementIn(source, parentOf(tree, up), up, force);
      if (!p) {
        return false;
      }
      sim = {.ax = p->x + p->r * sim.ax, .ay = p->y + p->r * sim.ay, .k = sim.k * p->r};
      up = parentOf(tree, up);
      --depthUp;
      return true;
    };
    while (depthUp > depthDown) {
      if (!climb()) {
        return std::nullopt;
      }
    }
    for (; depthDown > depthUp; --depthDown) {
      downs.push_back(down);
      down = parentOf(tree, down);
    }
    for (std::size_t guard = 0; up != down; ++guard) {
      if (guard >= tree.nodes.size() || !climb()) {
        return std::nullopt;
      }
      downs.push_back(down);
      down = parentOf(tree, down);
    }
    for (const auto node : downs | std::views::reverse) {
      const auto p = placementIn(source, parentOf(tree, node), node, force);
      if (!p) {
        return std::nullopt;
      }
      sim = {.ax = (sim.ax - p->x) / p->r, .ay = (sim.ay - p->y) / p->r, .k = sim.k / p->r};
    }
    return sim;
  }

  std::optional<Camera> convertCamera(const AtlasSource& source, NodeId from, NodeId to, const Camera& camera) {
    if (treeDistance(source.tree, from, to) > kNearSteps) {
      return std::nullopt;
    }
    const auto sim = similarity(source, from, to);
    if (!sim) {
      return std::nullopt;
    }
    return convert(camera, *sim);
  }

  double referenceLength(ViewSize view) {
    const double side = std::min(view.width, view.height);
    return std::isfinite(side) ? std::max(1.0, side) : 1.0;
  }

  double restScale(ViewSize view) {
    constexpr double kPad = 14;
    constexpr double kLeast = 40;
    const double r = std::min(view.width, view.height) / 2 - kPad;
    return std::isfinite(r) ? std::max(kLeast, r) : kLeast;
  }

  Camera restCamera(ViewSize view) { return {.cx = 0, .cy = 0, .scale = restScale(view)}; }
} // namespace luciole::workbench
