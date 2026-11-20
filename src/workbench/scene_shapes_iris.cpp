#include "workbench/iris_facts.h"
#include "workbench/iris_overlay_shapes.h"
#include "workbench/iris_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench/scene_style.h"

#include <algorithm>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The ring diagram: sectors with gaps, the inner band, the hub, label rows with dots and leaders, and marks; the
// text around the dial comes from iris_overlay_shapes.
namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kSectorAreaPerDegree = 100; // label order of ring sectors: a degree counts as this much area
    constexpr double kArcFont = 11;
    constexpr double kLeastSpan = 0.05; // degrees; a narrower sector is not drawn
    // A label row: padding, dot, gap, icon room and metric column; a compact row has no icon.
    constexpr double kRowPad = 8;
    constexpr double kRowDot = 8;
    constexpr double kRowIcon = 18;
    constexpr double kRowMetric = 72;
    constexpr double kRowFont = 13;
    constexpr double kCompactPad = 6;
    constexpr double kCompactMetric = 56;
    constexpr double kCompactFont = 11;

    // A sector with `gap` px cut from each side at the middle radius; a narrow one keeps at least a third of its span.
    Shape cutSector(ShapeId id, Point center, double r0, double r1, ring::Arc arc, double gap) {
      const double middle = (r0 + r1) / 2;
      double cut = middle > 0 ? gap / 2 / middle * 180 / std::numbers::pi : 0;
      if (arc.a1 - arc.a0 < 3 * cut) {
        cut = (arc.a1 - arc.a0) / 3;
      }
      auto shape = sectorShape(id, center, r0, r1, arc.a0 + cut, arc.a1 - cut);
      shape.hittable = false;
      return shape;
    }

    FileCategory categoryOf(const Tree& tree, NodeId node) {
      const std::size_t category = hasNode(tree, node) ? tree.nodes[node].category : 0;
      return category < kFileCategoryCount ? static_cast<FileCategory>(category) : FileCategory::Other;
    }

    ShapeId sectorId(const iris::SectorRing& ring, const iris::Sector& sector, SceneRole entryRole) {
      return sector.kind == iris::SectorKind::Entry ? sid(sector.node, entryRole) : sid(ring.folder, SceneRole::More);
    }

    void addRing(Builder& b, const iris::SectorRing& ring, Point center, double r0, double r1, bool main) {
      const auto& p = b.palette;
      for (const auto& sector : ring.sectors) {
        if (sector.arc.a1 - sector.arc.a0 < kLeastSpan) {
          continue;
        }
        const auto id = sectorId(ring, sector, SceneRole::Piece);
        const bool hot = b.hovered(id);
        auto shape = cutSector(id, center, r0, r1, sector.arc, main ? kIrisMainGap : kIrisContextGap);
        shape.stroke = tone(p.onSurfaceVariant, kHatch); // the hatch, without an outline
        const auto& color = sector.color;
        if (sector.kind != iris::SectorKind::Entry) {
          shape.fill = opaque(hot ? p.outlineVariant : p.surfaceContainerHighest);
          shape.pattern = ShapePattern::Hatched;
        } else if (main) {
          shape.fill = opaque(hot ? color.accent : color.fill);
        } else {
          shape.fill = hot    ? tone(color.accent, kIrisContextHot)
              : sector.onPath ? opaque(color.fill)
                              : tone(color.fill, kIrisContextFill);
          if (sector.onPath) {
            shape.stroke = opaque(color.accent);
            shape.strokeWidth = static_cast<float>(kIrisPathEdgeWidth);
          }
        }
        if (sector.kind == iris::SectorKind::Entry && hatched(b.tree, sector.node)) {
          shape.pattern = ShapePattern::Hatched;
        }
        b.put(shape);
        b.put(hitOnly(sectorShape(id, center, r0, r1, sector.arc.a0, sector.arc.a1)));
      }
    }

    // A row of a label column: a raised background when hovered, its dot, its leader and its two labels.
    void addRow(Builder& b, const iris::Scene& scene, const iris::LabelRow& row, bool cursor) {
      const auto& p = b.palette;
      const auto& main = scene.main;
      const auto& sector = main.sectors[row.sector];
      const bool entry = sector.kind == iris::SectorKind::Entry;
      const NodeId node = entry ? sector.node : main.folder;
      const auto rowId = sid(node, SceneRole::Row);
      const bool hot = b.hovered(rowId) || b.hovered(sectorId(main, sector, SceneRole::Piece));
      const double top = row.y - ring::kRowHeight / 2;
      auto shape = rectShape(rowId, row.box.x, top, row.box.width, ring::kRowHeight, kIrisRowRadius);
      shape.fill = tone(p.surfaceContainerHigh, hot || cursor ? 1.0 : 0.0);
      shape.stroke = opaque(p.primary);
      shape.strokeWidth = cursor ? 1.0F : 0.0F;
      shape.hittable = false;
      b.put(shape);
      b.put(hitArea(rowId, row.box.x, top, row.box.width, ring::kRowHeight));
      const bool compact = scene.dial.compact;
      const bool right = row.side == ring::Side::Right;
      const double pad = compact ? kCompactPad : kRowPad;
      const double dotX = right ? row.box.x + pad + kRowDot / 2 : row.box.x + row.box.width - pad - kRowDot / 2;
      auto dot = circleShape(sid(node, SceneRole::Detail), {.x = dotX, .y = row.y}, kRowDot / 2);
      dot.hittable = false;
      dot.fill = entry ? opaque(sector.color.accent) : tone(p.secondary, 0);
      dot.stroke = opaque(p.secondary);
      dot.strokeWidth = entry ? 0.0F : 1.0F;
      dot.pattern = entry ? ShapePattern::Solid : ShapePattern::DashedOutline;
      b.put(dot);
      const Rgb accent = entry ? sector.color.accent : p.secondary;
      const bool marked = hot || cursor;
      const Rgba8 strong = opaque(cursor ? p.primary : accent);
      const double width = cursor ? kIrisLeaderCursorWidth : marked ? kIrisLeaderMarkedWidth : 1.0;
      const std::size_t steps = row.leader.empty() ? 0 : row.leader.size() - 1;
      for (std::size_t k = 0; k < steps; ++k) {
        const auto& from = row.leader[k];
        const auto& to = row.leader[k + 1];
        const bool end = k == 0 || k + 1 == steps;
        const auto color = marked ? strong : tone(p.onSurfaceVariant, end ? kIrisLeaderEnd : kIrisLeaderMiddle);
        b.line(sid(node, SceneRole::Leader, k), {.x = from.x, .y = from.y}, {.x = to.x, .y = to.y}, color, width);
      }
      if (steps > 0) {
        const auto& start = row.leader.front();
        const double radius = marked ? kIrisLeaderMarkedDot : kIrisLeaderDotRadius;
        auto end = circleShape(sid(node, SceneRole::Detail, 1), {.x = start.x, .y = start.y}, radius);
        end.hittable = false;
        end.fill = marked ? strong : tone(p.onSurfaceVariant, kIrisLeaderDot);
        b.put(end);
      }
      const double area = (sector.arc.a1 - sector.arc.a0) * kSectorAreaPerDegree;
      const double metricWidth = compact ? kCompactMetric : kRowMetric;
      const double nameFrom = pad + kRowDot + pad + (compact ? 0 : kRowIcon + pad);
      const double nameX = right ? row.box.x + nameFrom : row.box.x + row.box.width - nameFrom - row.nameWidth;
      const double metricX = right ? row.box.x + row.box.width - pad - metricWidth : row.box.x + pad;
      const auto nameInk = entry ? p.onSurface : p.secondary;
      const auto nameAlign = right ? LabelAlign::Start : LabelAlign::End;
      auto* name = b.label(
          node, row.name, nameX, row.y, row.nameWidth, nameInk, area, nameAlign, compact ? kCompactFont : kRowFont
      );
      if (name != nullptr && sector.kind == iris::SectorKind::More) {
        name->fit = LabelFit::Whole; // "+N more" is a count
      }
      if (name != nullptr && entry && !compact) {
        name->icon = {
            .category = categoryOf(b.tree, node),
            .size = static_cast<float>(kRowIcon),
            .left = static_cast<float>(right ? -(kRowIcon + pad) : row.nameWidth + pad),
            .top = static_cast<float>((name->height - kRowIcon) / 2),
            .color = opaque(sector.color.accent),
        };
      }
      const auto metricAlign = right ? LabelAlign::End : LabelAlign::Start;
      if (auto* metric = b.label(
              node, row.metric, metricX, row.y, metricWidth, p.onSurfaceVariant, area, metricAlign, kDetailFont
          )) {
        metric->fit = LabelFit::Whole;
      }
    }

    // Where a node shows on the dial: its sector's angles and the radii of its band, main ring first.
    struct DialSpot {
      ring::Arc arc;
      double r0 = 0;
      double r1 = 0;
      NodeId node = kNoNode;
    };

    std::optional<DialSpot> spotOf(const iris::Scene& scene, NodeId node, bool contextToo) {
      if (node == kNoNode) {
        return std::nullopt;
      }
      const auto find = [node](const iris::SectorRing& ring, double r0, double r1) -> std::optional<DialSpot> {
        for (const auto& sector : ring.sectors) {
          if (marked(node, sector.node, sector.merged)) {
            const bool entry = sector.kind == iris::SectorKind::Entry;
            return DialSpot{.arc = sector.arc, .r0 = r0, .r1 = r1, .node = entry ? sector.node : ring.folder};
          }
        }
        return std::nullopt;
      };
      if (auto spot = find(scene.main, scene.dial.rIn, scene.dial.rOut)) {
        return spot;
      }
      for (std::size_t i = 0; contextToo && i < scene.rings.size(); ++i) {
        if (auto spot = find(scene.rings[i], scene.rings[i].band.r0, scene.rings[i].band.r1)) {
          return spot;
        }
      }
      return std::nullopt;
    }

    // An outline of `width` around a sector, 1 px outside its band.
    void addSectorMark(Builder& b, Point center, const DialSpot& spot, Rgba8 color, bool dashed) {
      const double grow = 1 + kIrisMarkWidth / 2;
      auto shape =
          cutSector(sid(spot.node, SceneRole::Mark), center, spot.r0 - grow, spot.r1 + grow, spot.arc, kIrisMarkGap);
      shape.fill.a = 0;
      shape.stroke = color;
      shape.strokeWidth = static_cast<float>(kIrisMarkWidth);
      shape.pattern = dashed ? ShapePattern::DashedOutline : ShapePattern::Solid;
      b.put(shape);
    }

    // The main ring's "+N more", where the keyboard cursor can rest.
    std::optional<DialSpot> moreSpot(const iris::Scene& scene) {
      for (const auto& sector : scene.main.sectors) {
        if (sector.kind == iris::SectorKind::More) {
          return DialSpot{.arc = sector.arc, .r0 = scene.dial.rIn, .r1 = scene.dial.rOut, .node = scene.main.folder};
        }
      }
      return std::nullopt;
    }

    // The folder whose entries the inner band shows: the pointed entry of the folder, else the cursor's.
    NodeId bandNode(const Tree& tree, const iris::Scene& scene, const ShapeMarks& marks, iris::CursorStop stop) {
      NodeId pointed = marks.linked;
      if (const auto& hovered = marks.hovered) {
        const auto role = sceneRole(hovered->role);
        pointed = role == SceneRole::Piece || role == SceneRole::Row ? hovered->node : pointed;
      }
      if (parentOf(tree, pointed) == scene.folder) {
        return pointed;
      }
      return stop == iris::CursorStop::More ? kNoNode : marks.cursor;
    }
  } // namespace

  SceneShapes irisShapes(
      const Tree& tree, const iris::Scene& scene, const ShapeMarks& marks, const ColorPalette& palette,
      iris::CursorStop stop
  ) {
    // A hovered label row marks the sector it names: the merged sector's row carries the folder's node.
    ShapeMarks sectorMarks = marks;
    if (const auto& row = marks.hovered; row && sceneRole(row->role) == SceneRole::Row) {
      const auto role = row->node == scene.main.folder ? SceneRole::More : SceneRole::Piece;
      sectorMarks.hovered = sid(row->node, role);
    }
    Builder b{.tree = tree, .marks = sectorMarks, .palette = palette, .out = {}, .labels = {}};
    const auto& dial = scene.dial;
    const Point center{.x = dial.center.x, .y = dial.center.y};
    addRing(b, scene.main, center, dial.rIn, dial.rOut, true);
    for (const auto& ring : scene.rings) {
      addRing(b, ring, center, ring.band.r0, ring.band.r1, false);
    }
    if (const auto band = iris::innerBand(tree, scene, bandNode(tree, scene, marks, stop), scene.settings, palette)) {
      for (const auto& shape : irisInnerBandShapes(scene, *band, palette)) {
        b.put(shape);
      }
    }
    auto hub = circleShape(sid(scene.folder, SceneRole::Hub), center, dial.rHub);
    const bool hotHub = b.hovered(hub.id);
    hub.fill = opaque(hotHub ? palette.surfaceContainer : palette.surfaceContainerLow);
    hub.stroke = opaque(hotHub ? palette.primary : palette.outlineVariant);
    hub.strokeWidth = 1;
    b.put(hub);
    const bool onMore = stop == iris::CursorStop::More;
    const auto cursorSpot = onMore ? moreSpot(scene) : spotOf(scene, marks.cursor, false);
    for (const auto* rows : {&scene.rows.right, &scene.rows.left}) {
      for (const auto& row : *rows) {
        if (row.shown && row.sector < scene.main.sectors.size()) {
          const auto& sector = scene.main.sectors[row.sector];
          const bool cursor = onMore ? sector.kind == iris::SectorKind::More
                                     : cursorSpot && marked(marks.cursor, sector.node, sector.merged);
          addRow(b, scene, row, cursor);
        }
      }
    }
    for (const auto& arc : scene.arcLabels) {
      if (arc.sector < scene.main.sectors.size()) {
        const auto& s = scene.main.sectors[arc.sector];
        const NodeId node = s.kind == iris::SectorKind::Entry ? s.node : scene.main.folder;
        const double area = (s.arc.a1 - s.arc.a0) * kSectorAreaPerDegree;
        // A hovered arc is filled with its accent, so its name takes the ink that reads on the accent.
        const bool hot = s.kind == iris::SectorKind::Entry && b.hovered(sid(s.node, SceneRole::Piece));
        const auto ink = hot ? inkFor(s.color.accent, palette) : arc.ink;
        b.label(
            node, arc.text, arc.center.x - arc.width / 2, arc.center.y, arc.width, ink, area, LabelAlign::Center,
            kArcFont
        );
      }
    }
    addIrisOverlay(b, scene, scene.readout ? iris::readoutLine(tree, scene, marks, stop) : iris::ReadoutLine{});
    if (cursorSpot) {
      addSectorMark(b, center, *cursorSpot, opaque(palette.primary), false);
    }
    if (const auto linked = spotOf(scene, marks.linked, true)) {
      addSectorMark(b, center, *linked, opaque(palette.tertiary), true);
    }
    return b.finish();
  }

  std::vector<Shape>
  irisInnerBandShapes(const iris::Scene& scene, const iris::InnerBand& band, const ColorPalette& palette) {
    const Point center{.x = scene.dial.center.x, .y = scene.dial.center.y};
    std::vector<Shape> shapes;
    shapes.reserve(band.sectors.size() + 1);
    auto backing =
        cutSector(sid(kNoNode, SceneRole::Detail), center, band.backing.r0, band.backing.r1, band.arc, kIrisBackingGap);
    backing.fill = tone(palette.surface, kIrisBacking);
    shapes.push_back(backing);
    for (const auto& sector : band.sectors) {
      const bool entry = sector.kind == iris::SectorKind::Entry;
      auto shape = cutSector(
          sid(entry ? sector.node : kNoNode, SceneRole::Detail), center, band.band.r0, band.band.r1, sector.arc,
          kIrisBandGap
      );
      shape.fill = opaque(entry ? sector.color.accent : palette.outlineVariant);
      shapes.push_back(shape);
    }
    return shapes;
  }
} // namespace luciole::workbench
