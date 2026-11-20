#include "workbench/atlas_label_shapes.h"
#include "workbench/scene_shapes.h"
#include "workbench/scene_style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

// The Atlas: circles, their state marks and labels, and the chips and zoom buttons over it.
namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kIconBox = 24;   // icons are drawn in a 24 unit box
    constexpr double kChipBorder = 1; // a chip's text box reaches its border: drawn text runs wider than measured

    // How strongly an ancestor rim shows: 1 for the parent, fainter outward.
    double ancestorStrength(const SceneItem& item) {
      const double level = static_cast<double>(std::max<std::size_t>(item.level, 1));
      return 1 / (1 + kAtlasAncestorFalloff * (level - 1));
    }

    // A drawn circle sits slightly inside its radius so touching circles stay apart.
    double drawnRadius(const SceneItem& item) {
      if (item.role == BubbleRole::Ancestor || item.role == BubbleRole::Self) {
        return item.r;
      }
      return item.r - std::min(kAtlasInsetMost, item.r * kAtlasInsetShare);
    }

    void styleItem(const Tree& tree, const ColorPalette& p, const SceneItem& item, NodeColor color, Shape& shape) {
      shape.pattern = ShapePattern::Solid;
      switch (item.role) {
      case BubbleRole::Ancestor: {
        const double strength = ancestorStrength(item);
        shape.fill = tone(p.surfaceContainerLow, kAtlasAncestorFill * strength);
        shape.stroke = tone(p.outlineVariant, kAtlasAncestorEdge * strength);
        shape.strokeWidth = item.huge ? 0.0F : 1.0F;
        return;
      }
      case BubbleRole::Self:
        shape.fill = tone(p.surfaceContainer, kAtlasSelfFill);
        shape.stroke = tone(p.secondary, kAtlasSelfEdge);
        shape.strokeWidth = static_cast<float>(kAtlasSelfEdgeWidth);
        return;
      case BubbleRole::More:
        shape.fill = tone(p.surfaceContainerHighest, kAtlasMoreFill);
        shape.stroke = tone(p.onSurfaceVariant, kAtlasMoreEdge);
        shape.strokeWidth = static_cast<float>(kAtlasMoreEdgeWidth);
        shape.pattern = ShapePattern::DashedOutline;
        return;
      default:
        break;
      }
      if (!hasNode(tree, item.node) || !tree.nodes[item.node].folder) {
        shape.fill = tone(color.fill, kAtlasFileFill);
        shape.stroke = tone(color.accent, kAtlasFolderEdge);
        shape.pattern = hatched(tree, item.node) ? ShapePattern::Hatched : ShapePattern::Solid;
        return;
      }
      const auto& node = tree.nodes[item.node];
      shape.fill = tone(color.fill, kAtlasFolderFill);
      shape.stroke = tone(color.accent, kAtlasFolderEdge);
      shape.strokeWidth = static_cast<float>(drawnRadius(item) > 10 ? kAtlasFolderEdgeWidth : kAtlasSmallEdgeWidth);
      shape.pattern = hatched(tree, item.node) ? ShapePattern::Hatched
          : node.truncated || node.unreadable  ? ShapePattern::DashedOutline
                                               : ShapePattern::Solid;
    }

    // The hovered, linked and cursor outlines, over every circle.
    void addAtlasMarks(
        Builder& b, const AtlasScene& scene, std::optional<std::size_t> linked, std::optional<std::size_t> cursor
    ) {
      const auto& p = b.palette;
      for (std::size_t i = 0; i < scene.items.size(); ++i) {
        const auto& item = scene.items[i];
        if (item.node == kNoNode || item.fade < kAtlasMarkFrom || item.huge) {
          continue;
        }
        const ShapePoint center{.x = static_cast<float>(item.x), .y = static_cast<float>(item.y)};
        const auto id = sid(item.node, SceneRole::Mark);
        const bool quiet = item.role == BubbleRole::Self || item.role == BubbleRole::Ancestor;
        if (!quiet && b.hovered(sid(item.node, SceneRole::Piece))) {
          auto ring = ringCircle(id, center, item.r, tone(p.onSurface, kAtlasHotEdge), kAtlasHotWidth);
          ring.fill = tone(p.onSurface, kAtlasHotFill);
          b.put(ring);
        }
        if (linked == i) {
          const auto color = tone(p.tertiary, kAtlasFocusEdge);
          auto ring = ringCircle(id, center, item.r + kAtlasFocusGrow, color, kAtlasFocusWidth);
          ring.fill = tone(p.tertiary, kAtlasFocusFill);
          ring.pattern = ShapePattern::DashedOutline;
          b.put(ring);
        }
        if (cursor == i && item.role == BubbleRole::Bubble) {
          const double radius = std::max(item.r + kAtlasCursorGrow, kAtlasCursorLeast);
          b.put(ringCircle(id, center, radius, opaque(p.primary), kAtlasCursorWidth));
        }
      }
    }

    // An icon stroke from (u0, v0) to (u1, v1) in a 24 unit box centered in `box`.
    void iconLine(Builder& b, ShapeId id, const Rect& box, std::array<double, 4> uv, Rgba8 color) {
      const double scale = kHudIconSize / kIconBox;
      const double cx = box.x + box.width / 2;
      const double cy = box.y + box.height / 2;
      const auto at = [&](double u, double v) {
        return Point{.x = cx + (u - kIconBox / 2) * scale, .y = cy + (v - kIconBox / 2) * scale};
      };
      b.line(id, at(uv[0], uv[1]), at(uv[2], uv[3]), color, kHudIconWidth);
    }

    // Zoom in, zoom out and recenter, each a raised button holding its icon.
    void addHudButtons(Builder& b, const HudLayout& layout) {
      const auto& p = b.palette;
      for (std::size_t k = 0; k < layout.buttons.size(); ++k) {
        const auto& box = layout.buttons[k];
        const double radius = layout.compact ? kHudChipRadius : kHudButtonRadius;
        auto shape = rectShape(sid(kNoNode, SceneRole::Button, k), box.x, box.y, box.width, box.height, radius);
        const bool hot = b.hovered(shape.id);
        shape.fill = hot ? opaque(p.surfaceContainerHigh) : tone(p.surface, kHudChipFill);
        shape.stroke = opaque(p.outlineVariant);
        shape.strokeWidth = 1;
        b.put(shape);
        const auto ink = opaque(hot ? p.onSurface : p.onSurfaceVariant);
        const auto id = sid(kNoNode, SceneRole::Detail, k);
        if (k == 0) {
          iconLine(b, id, box, {12, 5, 12, 19}, ink);
        }
        if (k <= 1) {
          iconLine(b, id, box, {5, 12, 19, 12}, ink);
          continue;
        }
        for (const auto& tick :
             {std::array<double, 4>{12, 3, 12, 7}, {12, 17, 12, 21}, {3, 12, 7, 12}, {17, 12, 21, 12}}) {
          iconLine(b, id, box, tick, ink);
        }
        const ShapePoint center{
            .x = static_cast<float>(box.x + box.width / 2), .y = static_cast<float>(box.y + box.height / 2)
        };
        const double scale = kHudIconSize / kIconBox;
        b.put(ringCircle(id, center, 3 * scale, ink, kHudIconWidth));
      }
    }
  } // namespace

  SceneShapes atlasShapes(
      const Tree& tree, const LayoutSettings& settings, const AtlasScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette, const LabelFont& font
  ) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    Colors colors{.tree = tree, .settings = settings, .palette = palette, .ranges = {}};
    const auto focus = [&](NodeId id) { return id == kNoNode ? std::nullopt : findFocus(scene, tree, id); };
    const auto view = scene.view;
    RankedShapes underLabels;
    for (const auto& item : scene.items) {
      const bool more = item.role == BubbleRole::More;
      const auto id = more ? sid(item.folder, SceneRole::More) : sid(item.node, SceneRole::Piece);
      const auto color = colors.of(item.node);
      const auto shapeAt = [&](double radius) {
        return item.huge ? rectShape(id, 0, 0, view.width, view.height)
                         : circleShape(id, {.x = item.x, .y = item.y}, radius);
      };
      auto shape = shapeAt(drawnRadius(item));
      styleItem(tree, palette, item, color, shape);
      shape.opacity = static_cast<float>(item.fade);
      shape.hittable = false;
      b.put(shape);
      if (item.clickable) {
        auto hit = hitOnly(shapeAt(item.r));
        hit.opacity = static_cast<float>(item.fade);
        b.put(hit);
      }
      addAtlasLabel(b, settings, item, view, color, font, underLabels);
    }
    addAtlasMarks(b, scene, focus(marks.linked), focus(marks.cursor));
    putKeptLabelShapes(b, underLabels);
    return b.finish();
  }

  SceneShapes atlasHudShapes(
      const Tree& tree, const HudLayout& layout, std::span<const Rect> placed, const ShapeMarks& marks,
      const ColorPalette& palette
  ) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    const double pad = layout.compact ? kAtlasBarChipPad : kAtlasChipPad;
    for (std::size_t i = 0; i < std::min(layout.entries.size(), placed.size()); ++i) {
      const auto& entry = layout.entries[i];
      const auto& rect = placed[i];
      const double middle = rect.y + rect.height / 2;
      const double area = rect.width * rect.height;
      if (entry.kind == HudEntryKind::MoreLevels) {
        b.label(
            kNoNode, hudText(tree, entry), rect.x + kAtlasMoreLevelsPad, middle, rect.width - kAtlasMoreLevelsPad,
            palette.onSurfaceVariant, area, LabelAlign::Start, entry.fontSize
        );
        continue;
      }
      const bool current = entry.kind == HudEntryKind::Current;
      const std::size_t up = entry.levels - 1;
      const NodeId node = current ? tree.root : up < tree.ancestors.size() ? tree.ancestors[up] : kNoNode;
      if (!hasNode(tree, node)) {
        continue;
      }
      auto chip = rectShape(sid(node, SceneRole::Chip), rect.x, rect.y, rect.width, rect.height, kHudChipRadius);
      const bool hot = !current && b.hovered(chip.id);
      chip.fill = hot ? opaque(palette.surfaceContainerHigh) : tone(palette.surface, kHudChipFill);
      chip.stroke = opaque(hot || current ? palette.secondary : palette.outlineVariant);
      chip.strokeWidth = 1;
      chip.opacity = static_cast<float>(entry.opacity);
      chip.hittable = !current;
      b.put(chip);
      const auto ink = hot || current ? palette.onSurface : palette.onSurfaceVariant;
      if (auto* label = b.label(
              node, hudText(tree, entry), rect.x + pad, middle, rect.width - pad - kChipBorder, ink, area,
              LabelAlign::Start, entry.fontSize
          )) {
        label->color = tone(ink, entry.opacity);
      }
    }
    addHudButtons(b, layout);
    return b.finish();
  }
} // namespace luciole::workbench
