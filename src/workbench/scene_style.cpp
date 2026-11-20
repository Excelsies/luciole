#include "workbench/scene_style.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numeric>
#include <vector>

namespace luciole::workbench::scene_style {
  namespace {
    constexpr double kLineBox = 1.3; // a label box is this many font sizes tall
  } // namespace

  Rgba8 tone(Rgb color, double alpha) {
    const double unit = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
    return {.r = color.r, .g = color.g, .b = color.b, .a = static_cast<std::uint8_t>(std::lround(unit * 255))};
  }

  Rgba8 opaque(Rgb color) { return tone(color, 1); }

  ShapeId sid(NodeId node, SceneRole role, std::size_t part) { return {.node = node, .role = shapeRole(role, part)}; }

  Shape hitArea(ShapeId id, double x, double y, double width, double height) {
    return hitOnly(rectShape(id, x, y, width, height));
  }

  Shape hitOnly(Shape shape) {
    shape.fill.a = 0;
    shape.stroke.a = 0;
    shape.strokeWidth = 0;
    shape.pattern = ShapePattern::Solid;
    shape.hittable = true;
    return shape;
  }

  Shape ringAround(ShapeId id, const ShapeRect& box, double grow, double radius, Rgba8 color, double width) {
    const double outer = grow + width / 2;
    auto shape = rectShape(
        id, box.x - outer, box.y - outer, box.width + 2 * outer, box.height + 2 * outer,
        std::max(0.0, radius + width / 2)
    );
    shape.fill.a = 0;
    shape.stroke = color;
    shape.strokeWidth = static_cast<float>(width);
    shape.hittable = false;
    return shape;
  }

  Shape ringCircle(ShapeId id, ShapePoint center, double radius, Rgba8 color, double width) {
    auto shape = circleShape(id, {.x = center.x, .y = center.y}, radius + width / 2);
    shape.fill.a = 0;
    shape.stroke = color;
    shape.strokeWidth = static_cast<float>(width);
    shape.hittable = false;
    return shape;
  }

  bool marked(NodeId mark, NodeId piece, std::span<const NodeId> merged) {
    return mark != kNoNode && (mark == piece || std::ranges::contains(merged, mark));
  }

  bool hatched(const Tree& tree, NodeId id) {
    return hasNode(tree, id) && (tree.nodes[id].partial || tree.nodes[id].lowerBound);
  }

  std::string nodeName(const Tree& tree, NodeId id) { return hasNode(tree, id) ? tree.nodes[id].name : std::string(); }

  bool Builder::hovered(ShapeId id) const {
    if (!marks.hovered || marks.hovered->node != id.node) {
      return false;
    }
    const auto role = sceneRole(id.role);
    if (role == SceneRole::Files || role == SceneRole::Button) {
      return marks.hovered->role == id.role;
    }
    return sceneRole(marks.hovered->role) == role;
  }

  Shape& Builder::put(Shape shape) {
    out.shapes.push_back(shape);
    return out.shapes.back();
  }

  void Builder::line(ShapeId id, Point from, Point to, Rgba8 color, double width) {
    auto shape = lineShape(id, from, to, width);
    shape.hittable = false;
    shape.fill = color;
    out.shapes.push_back(shape);
  }

  ShapeLabel* Builder::label(
      NodeId node, std::string text, double x, double middle, double width, Rgb color, double area, LabelAlign align,
      double size
  ) {
    if (text.empty() || !(width > 0)) {
      return nullptr;
    }
    const auto f = [](double value) { return static_cast<float>(value); };
    ShapeLabel item{
        .node = node,
        .text = std::move(text),
        .x = f(x),
        .y = f(middle - size * kLineBox / 2),
        .width = f(width),
        .height = f(size * kLineBox),
        .align = align,
        .fontSize = f(size),
        .rotation = 0,
        .color = opaque(color),
    };
    return &labels.emplace_back(area, std::move(item)).second;
  }

  SceneShapes Builder::finish() {
    // The labels of the largest areas, the earlier first among equals: ranked by index, moved once.
    std::vector<std::size_t> order(labels.size());
    std::ranges::iota(order, std::size_t{0});
    const std::size_t kept = std::min(labels.size(), kMaxSceneLabels);
    std::ranges::partial_sort(
        order, order.begin() + static_cast<std::ptrdiff_t>(kept), [this](std::size_t a, std::size_t b) {
          return labels[a].first != labels[b].first ? labels[a].first > labels[b].first : a < b;
        }
    );
    out.labels.reserve(kept);
    for (std::size_t i = 0; i < kept; ++i) {
      out.labels.push_back(std::move(labels[order[i]].second));
    }
    return std::move(out);
  }

  NodeColor Colors::of(NodeId id) {
    if (!hasNode(tree, id) || !hasNode(tree, tree.nodes[id].parent)) {
      return nodeColor(tree, id, settings, palette);
    }
    const auto parent = tree.nodes[id].parent;
    const auto [entry, fresh] = ranges.try_emplace(parent);
    if (fresh) {
      entry->second = colorRange(tree, parent, settings.colorBy, settings.showHidden);
    }
    return nodeColor(tree, id, settings.colorBy, entry->second, palette);
  }
} // namespace luciole::workbench::scene_style
