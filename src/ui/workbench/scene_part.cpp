#include "ui/workbench/scene_part.h"

#include "core/input/key_symbols.h"
#include "render/core/renderer.h"
#include "ui/controls/shape_labels.h"
#include "workbench/iris_scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

namespace luciole {
  namespace {
    using workbench::NodeId;
    using workbench::SceneRole;

    // Stands for every non-ASCII code point when widths are summed: the ellipsis a cut label gains, about one em.
    constexpr std::string_view kOtherSample = "…";
    constexpr float kNoteFont = 13;
    constexpr float kNoteBox = 1.3F; // a note's box is this many font sizes tall
    constexpr float kNotePad = 12;

    void moveShape(workbench::Shape& shape, float dx, float dy) {
      shape.rect.x += dx;
      shape.rect.y += dy;
      shape.sector.center.x += dx;
      shape.sector.center.y += dy;
      shape.circle.center.x += dx;
      shape.circle.center.y += dy;
      for (auto& point : shape.ribbon.top) {
        point.x += dx;
        point.y += dy;
      }
      for (auto& point : shape.ribbon.bottom) {
        point.x += dx;
        point.y += dy;
      }
      shape.line.from.x += dx;
      shape.line.from.y += dy;
      shape.line.to.x += dx;
      shape.line.to.y += dy;
    }
  } // namespace

  bool samePalette(const workbench::ColorPalette& left, const workbench::ColorPalette& right) {
    // Every byte is a color channel, so comparing the bytes compares every field.
    static_assert(std::has_unique_object_representations_v<workbench::ColorPalette>);
    return std::memcmp(&left, &right, sizeof(workbench::ColorPalette)) == 0;
  }

  void LabelMetrics::measure(Renderer& renderer) {
    const auto size = static_cast<float>(kBaseSize);
    const TextFont semibold{.weight = FontWeight::SemiBold, .family = {}, .features = {}};
    for (std::size_t code = 0; code < m_font.ascii.size(); ++code) {
      const char ch = static_cast<char>(code);
      const bool printable = code >= 0x20 && code < 0x7F;
      const std::string_view text(&ch, 1);
      m_font.ascii[code] = printable ? renderer.measureText(text, size).width : 0;
      m_semibold.ascii[code] = printable ? renderer.measureText(text, size, semibold).width : 0;
    }
    m_font.other = renderer.measureText(kOtherSample, size).width;
    m_semibold.other = renderer.measureText(kOtherSample, size, semibold).width;
    m_ready = true;
  }

  double LabelMetrics::width(std::string_view text, double size, workbench::LabelWeight weight) const {
    const auto& font = weight == workbench::LabelWeight::Semibold ? m_semibold : m_font;
    return workbench::labelTextWidth(text, font) * size / kBaseSize;
  }

  workbench::LabelFont LabelMetrics::font(double size, workbench::LabelWeight weight) const {
    workbench::LabelFont scaled = weight == workbench::LabelWeight::Semibold ? m_semibold : m_font;
    const double factor = size / kBaseSize;
    for (auto& advance : scaled.ascii) {
      advance *= factor;
    }
    scaled.other *= factor;
    return scaled;
  }

  LabelMetrics LabelMetrics::scaled(double factor) const {
    LabelMetrics out = *this;
    for (auto* font : {&out.m_font, &out.m_semibold}) {
      for (auto& advance : font->ascii) {
        advance *= factor;
      }
      font->other *= factor;
    }
    return out;
  }

  std::optional<PartRequest> clickRequest(const workbench::Tree& tree, workbench::ShapeId id) {
    switch (workbench::sceneRole(id.role)) {
    case SceneRole::More:
      return PartRequest{.kind = PartRequestKind::ShowMore, .node = id.node};
    case SceneRole::Hub: {
      const NodeId parent = workbench::parentOf(tree, id.node);
      if (!workbench::hasNode(tree, parent)) {
        return std::nullopt;
      }
      return PartRequest{.kind = PartRequestKind::Go, .node = parent};
    }
    case SceneRole::Piece:
    case SceneRole::Head:
    case SceneRole::Far:
    case SceneRole::Row:
    case SceneRole::Stem:
      if (!workbench::hasNode(tree, id.node) || id.node == tree.root) {
        return std::nullopt;
      }
      return PartRequest{
          .kind = tree.nodes[id.node].folder ? PartRequestKind::Go : PartRequestKind::Select, .node = id.node
      };
    default:
      return std::nullopt;
    }
  }

  NodeId hoveredNode(workbench::ShapeId id) {
    switch (workbench::sceneRole(id.role)) {
    case SceneRole::More:
    case SceneRole::Files:
    case SceneRole::Link:
    case SceneRole::Bridge:
    case SceneRole::Hub:
    case SceneRole::Stack:
      return workbench::kNoNode;
    default:
      return id.node;
    }
  }

  NodeId ScenePart::pointedNode(workbench::ShapeId id) const { return hoveredNode(id); }

  std::optional<PartRequest> enterRequest(const workbench::Tree& tree, NodeId cursor) {
    if (!workbench::hasNode(tree, cursor)) {
      return std::nullopt;
    }
    return PartRequest{.kind = tree.nodes[cursor].folder ? PartRequestKind::Go : PartRequestKind::Open, .node = cursor};
  }

  std::optional<PartRequest> upRequest(const workbench::Tree& tree) {
    const NodeId parent = workbench::parentOf(tree, tree.root);
    if (!workbench::hasNode(tree, parent)) {
      return std::nullopt;
    }
    return PartRequest{.kind = PartRequestKind::Go, .node = parent};
  }

  std::optional<KeyStep> arrowStep(std::uint32_t sym) {
    if (KeySymbol::isLeft(sym)) {
      return KeyStep{.dx = -1, .dy = 0};
    }
    if (KeySymbol::isRight(sym)) {
      return KeyStep{.dx = 1, .dy = 0};
    }
    if (KeySymbol::isUp(sym)) {
      return KeyStep{.dx = 0, .dy = -1};
    }
    if (KeySymbol::isDown(sym)) {
      return KeyStep{.dx = 0, .dy = 1};
    }
    return std::nullopt;
  }

  PartReaction LevelWheel::turn(
      const ShapeWheel& wheel, const workbench::Tree& tree, NodeId under, std::chrono::steady_clock::time_point now
  ) {
    if (!wheel.vertical || !std::isfinite(wheel.delta) || wheel.delta == 0) {
      return {};
    }
    PartReaction reaction{.handled = true, .relayout = false, .requests = {}};
    if (m_last && now - *m_last < kInterval) {
      return reaction;
    }
    NodeId target = workbench::kNoNode;
    if (wheel.delta < 0) {
      const auto parent = workbench::parentOf(tree, under);
      const bool below =
          parent == tree.root || (parent != workbench::kNoNode && workbench::parentOf(tree, parent) == tree.root);
      target = below && workbench::hasNode(tree, under) && tree.nodes[under].folder ? under : workbench::kNoNode;
    } else {
      target = workbench::parentOf(tree, tree.root);
    }
    if (workbench::hasNode(tree, target)) {
      m_last = now;
      reaction.requests.push_back({.kind = PartRequestKind::Go, .node = target});
    }
    return reaction;
  }

  void offsetScene(workbench::SceneShapes& scene, float dx, float dy) {
    for (auto& shape : scene.shapes) {
      moveShape(shape, dx, dy);
    }
    for (auto& label : scene.labels) {
      label.x += dx;
      label.y += dy;
    }
  }

  void fitLabels(std::vector<workbench::ShapeLabel>& labels, const LabelMetrics& metrics) {
    fitShapeLabels(labels, [&metrics](std::string_view text, double size, workbench::LabelWeight weight) {
      return metrics.width(text, size, weight);
    });
  }

  std::string folderNote(const workbench::Tree& tree, const workbench::LayoutSettings& settings) {
    if (!workbench::hasNode(tree, tree.root)) {
      return "Loading…";
    }
    const auto visible = std::ranges::count_if(workbench::children(tree, tree.root), [&](NodeId id) {
      return workbench::hasNode(tree, id) && workbench::visibleEntry(tree.nodes[id], settings.showHidden);
    });
    if (visible == 0 && tree.nodes[tree.root].partial) {
      return "Loading…";
    }
    return workbench::iris::hubNote(tree, tree.root, static_cast<std::size_t>(visible), {});
  }

  workbench::ShapeLabel
  noteLabel(std::string text, const workbench::ShapeRect& box, const workbench::ColorPalette& palette) {
    const auto& ink = palette.onSurface;
    return {
        .node = workbench::kNoNode,
        .text = std::move(text),
        .x = box.x + kNotePad,
        .y = box.y + (box.height - kNoteFont * kNoteBox) / 2,
        .width = std::max(box.width - 2 * kNotePad, 0.0F),
        .height = kNoteFont * kNoteBox,
        .align = workbench::LabelAlign::Center,
        .fontSize = kNoteFont,
        .rotation = 0,
        .color = {.r = ink.r, .g = ink.g, .b = ink.b, .a = 255},
    };
  }
} // namespace luciole
