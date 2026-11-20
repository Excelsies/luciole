#include "workbench/scene_shapes.h"

#include "workbench/scene_style.h"
#include "workbench/treemap.h"

#include <algorithm>
#include <array>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kBaselineToMiddle = 0.35; // a baseline sits this many font sizes below the line's middle
    constexpr double kMoreFont = 13;
    constexpr double kPeekNameFont = 13;
    constexpr double kPeekFactsGap = 8;
    constexpr double kNestMeasureGap = 14;     // between a one-line tile's name and its measure
    constexpr double kNestMoreSizeDrop = 18;   // from the middle of "+N more" to the middle of its size line
    constexpr double kNestMoreSizeInk = 0.8;   // secondary of the size line
    constexpr std::size_t kShapesPerPiece = 3; // a tile, its texture patches and its edge: room made up front

    // Rectangles for the straight pieces and sectors for the corner arcs of band stretch [s0, s1], between offsets
    // `inner` and `outer` from the centerline (outward positive).
    template <typename Emit>
    void bandParts(const Band& band, double s0, double s1, double inner, double outer, Emit emit) {
      constexpr std::array<std::array<double, 2>, 4> kNormals{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};
      const double sx = band.halfStraightX;
      const double sy = band.halfStraightY;
      const std::array<std::array<double, 3>, 4> corners{{
          {band.centerX + sx, band.centerY - sy, -std::numbers::pi / 2},
          {band.centerX + sx, band.centerY + sy, 0},
          {band.centerX - sx, band.centerY + sy, std::numbers::pi / 2},
          {band.centerX - sx, band.centerY - sy, std::numbers::pi},
      }};
      for (std::size_t i = 0; i < kBandPieces; ++i) {
        const double a = std::max(s0, band.starts[i]);
        const double b = std::min(s1, band.starts[i] + band.lengths[i]);
        if (b <= a) {
          continue;
        }
        if (i % 2 == 0) {
          const auto [nx, ny] = kNormals[i / 2];
          const auto p = pointAt(band, a);
          const auto q = pointAt(band, b);
          const auto [x0, x1] = std::minmax({p.x + nx * inner, p.x + nx * outer, q.x + nx * inner, q.x + nx * outer});
          const auto [y0, y1] = std::minmax({p.y + ny * inner, p.y + ny * outer, q.y + ny * inner, q.y + ny * outer});
          emit(rectShape({}, x0, y0, x1 - x0, y1 - y0));
        } else {
          const auto& [cx, cy, base] = corners[i / 2];
          const double rc = band.cornerRadius;
          const double from = (base + (a - band.starts[i]) / rc) / std::numbers::pi * 180 + 90;
          const double to = (base + (b - band.starts[i]) / rc) / std::numbers::pi * 180 + 90;
          emit(sectorShape({}, {.x = cx, .y = cy}, std::max(rc + inner, 0.0), rc + outer, from, to));
        }
      }
    }

    ShapeRect boxOf(const TreemapRect& r) {
      const auto f = [](double value) { return static_cast<float>(value); };
      return {.x = f(r.x), .y = f(r.y), .width = f(r.w), .height = f(r.h), .radius = 0};
    }

    bool mergedPiece(const NestPiece& piece) {
      const auto kind = piece.kind;
      return kind == NestPieceKind::More
          || kind == NestPieceKind::MergedSegment
          || kind == NestPieceKind::PeekMore
          || (kind == NestPieceKind::LineSegment && !piece.nodes.empty());
    }

    // A tile's semibold name with its icon, and its measure as a whole trail: under the name on a tall tile, at
    // the right end of a one-line tile.
    void addTileLabel(Builder& b, const NestPiece& piece, const NestTileLabel& tile, double area) {
      const double size = tile.fontSize;
      const double nameY = tile.nameAt.y - kBaselineToMiddle * size;
      auto* name =
          b.label(piece.node, tile.name, tile.nameAt.x, nameY, tile.nameWidth, piece.color.ink, area, {}, size);
      if (name == nullptr) {
        return;
      }
      const auto f = [](double value) { return static_cast<float>(value); };
      name->weight = LabelWeight::Semibold;
      if (tile.iconSize > 0) {
        name->icon = {
            .category = tile.category,
            .size = f(tile.iconSize),
            .left = f(tile.icon.x) - name->x,
            .top = f(tile.icon.y) - name->y,
            .color = name->color,
        };
      }
      LabelTrail trail{.text = tile.detail, .fontSize = f(kDetailFont), .color = name->color};
      if (tile.twoLines) {
        trail.place = TrailPlace::Below;
        trail.dx = f(tile.detailAt.x) - name->x;
        trail.dy = f(tile.detailAt.y - kBaselineToMiddle * kDetailFont - nameY);
        trail.width = f(tile.detailWidth);
      } else {
        trail.gap = f(kNestMeasureGap);
        trail.room = f(tile.detailWidth);
      }
      name->trail = std::move(trail);
    }

    void addNestLabel(Builder& b, const NestPiece& piece, double area) {
      const auto ink = piece.color.ink;
      if (const auto* tile = std::get_if<NestTileLabel>(&piece.label)) {
        addTileLabel(b, piece, *tile, area);
      } else if (const auto* more = std::get_if<NestMoreLabel>(&piece.label)) {
        const double x = more->center.x - more->maxWidth / 2;
        const auto& secondary = b.palette.secondary;
        auto* count = b.label(
            piece.node, more->text, x, more->center.y, more->maxWidth, secondary, area, LabelAlign::Center, kMoreFont
        );
        if (count == nullptr) {
          return;
        }
        count->fit = LabelFit::Whole;
        count->weight = LabelWeight::Semibold;
        count->shortText = more->shortText;
        if (!more->size.empty()) {
          count->trail = LabelTrail{
              .text = more->size,
              .place = TrailPlace::Below,
              .fontSize = static_cast<float>(kDetailFont),
              .color = tone(secondary, kNestMoreSizeInk),
              .dy = static_cast<float>(kNestMoreSizeDrop),
              .width = count->width,
          };
        }
      } else if (const auto* ring = std::get_if<NestRingLabel>(&piece.label)) {
        const double x = ring->center.x - ring->maxWidth / 2;
        const auto color = piece.onPath ? b.palette.primary : ink;
        auto* label = b.label(
            piece.node, ring->name, x, ring->center.y, ring->maxWidth, color, area, LabelAlign::Center, ring->fontSize
        );
        if (label == nullptr) {
          return;
        }
        label->weight = LabelWeight::Semibold;
        if (ring->turn != NestTurn::None) {
          label->rotation = ring->turn == NestTurn::Clockwise ? 90.0F : -90.0F;
        }
      }
    }

    // A "+N more" tile: a faint secondary fill with a dashed secondary outline.
    void addMoreTile(Builder& b, const NestPiece& piece, bool linked) {
      const auto& p = b.palette;
      const auto& r = piece.rect;
      auto shape = rectShape(sid(piece.node, SceneRole::More), r.x, r.y, r.w, r.h, kNestMoreRadius);
      const bool hot = b.hovered(shape.id);
      shape.fill = tone(p.secondary, hot ? kNestMoreFillHot : kNestMoreFill);
      shape.stroke = linked ? opaque(p.tertiary) : tone(p.secondary, hot ? kNestMoreEdgeHot : kNestMoreEdge);
      shape.strokeWidth = static_cast<float>(linked ? kNestFocusWidth : 1.0);
      shape.pattern = linked ? ShapePattern::Solid : ShapePattern::DashedOutline;
      b.put(shape);
      addNestLabel(b, piece, r.w * r.h);
    }

    // A core or peek tile: its fill with a faint accent outline; hovered brighter, linked tinted and outlined.
    void addTile(Builder& b, const NestPiece& piece, bool linked, bool peek) {
      const auto& p = b.palette;
      const auto& r = piece.rect;
      auto shape = rectShape(
          sid(piece.node, SceneRole::Piece), r.x, r.y, r.w, r.h, peek ? kNestPeekTileRadius : kNestTileRadius
      );
      const bool hot = b.hovered(shape.id);
      Rgb fill = peek ? mix(p.surfaceContainerHigh, piece.color.accent, kNestPeekTint) : piece.color.fill;
      fill = hot ? mix(fill, p.onSurface, kNestHotTint) : fill;
      fill = linked ? mix(fill, p.tertiary, kNestFocusTint) : fill;
      const double edge = hot ? kNestHotEdge : peek ? kNestPeekTileEdge : kNestTileEdge;
      shape.fill = opaque(fill);
      shape.stroke = linked ? opaque(p.tertiary) : tone(piece.color.accent, edge);
      shape.strokeWidth = static_cast<float>(linked ? kNestFocusWidth : 1.0);
      shape.pattern = hatched(b.tree, piece.node) ? ShapePattern::Hatched : ShapePattern::Solid;
      b.put(shape);
      addNestLabel(b, piece, r.w * r.h);
    }

    void addTexture(Builder& b, const NestPiece& tile, std::span<const NestPatch> patches) {
      for (std::size_t k = 0; k < patches.size(); ++k) {
        const auto& r = patches[k].rect;
        const double inset = kNestPatchInset;
        auto shape = rectShape(
            sid(tile.node, SceneRole::Detail, k), r.x + inset, r.y + inset, r.w - 2 * inset, r.h - 2 * inset,
            kNestPatchRadius
        );
        shape.fill = tone(patches[k].color.accent, kNestPatch);
        shape.hittable = false;
        b.put(shape);
      }
    }

    // The outline of a ring segment between s0 and s1, `width` wide and centered on its edges: each long edge as the
    // straight bands and arcs of the ring's band, and a line across each end, long enough to close the corners.
    void addSegmentEdge(
        Builder& b, const NestRing& ring, const NestPiece& piece, double s0, double s1, Rgba8 color, double width
    ) {
      const double half = ring.thickness / 2;
      std::size_t part = 0;
      const auto put = [&](Shape shape) {
        shape.id = sid(piece.node, SceneRole::Detail, part++);
        shape.fill = color;
        shape.hittable = false;
        b.put(shape);
      };
      for (const double edge : {half, -half}) {
        bandParts(ring.band, s0, s1, edge - width / 2, edge + width / 2, put);
      }
      const double reach = half + width / 2;
      for (const double s : {s0, s1}) {
        const auto p = pointAt(ring.band, s);
        const Point inner{.x = p.x - p.normalX * reach, .y = p.y - p.normalY * reach};
        const Point outer{.x = p.x + p.normalX * reach, .y = p.y + p.normalY * reach};
        b.line(sid(piece.node, SceneRole::Detail, part++), inner, outer, color, width);
      }
    }

    Rgba8 segmentFill(const ColorPalette& p, const NestRing& ring, const NestPiece& piece, bool merged) {
      const bool thin = ring.thickness <= kNestThinRing;
      if (merged) {
        return opaque(p.surfaceContainerHighest);
      }
      if (piece.onPath) {
        return thin ? tone(p.primary, ring.line ? kNestLinePath : kNestThinPath) : opaque(p.surfaceContainerHighest);
      }
      return thin ? tone(piece.color.accent, kNestThinFill) : opaque(piece.color.fill);
    }

    // A ring segment: the rectangles and corner sectors of its band, a bridge under it on the path, and an outline
    // for the strongest of linked, hovered and on-path.
    void addSegment(Builder& b, const NestScene& scene, const NestPiece& piece, bool linked) {
      const auto& p = b.palette;
      const auto& ring = scene.rings[piece.ring];
      const bool merged = mergedPiece(piece);
      const auto role = merged ? SceneRole::More : SceneRole::Piece;
      const bool hot = b.hovered(sid(piece.node, role));
      auto fill = segmentFill(p, ring, piece, merged);
      const Rgb tinted =
          hot ? mix({fill.r, fill.g, fill.b}, p.onSurface, kNestRingHotTint) : Rgb{fill.r, fill.g, fill.b};
      const Rgb focused = linked ? mix(tinted, p.tertiary, kNestFocusTint) : tinted;
      fill = {.r = focused.r, .g = focused.g, .b = focused.b, .a = fill.a};
      const bool striped = merged || hatched(b.tree, piece.node);
      const double half = ring.thickness / 2;
      const double inset = piece.s1 - piece.s0 > 10 ? 1.0 : 0.4;
      const double s0 = piece.s0 + inset;
      const double s1 = piece.s1 - inset;
      std::size_t part = 0;
      bandParts(ring.band, s0, s1, -half, half, [&](Shape shape) {
        shape.id = sid(piece.node, role, part++);
        shape.fill = fill;
        shape.stroke = tone(p.onSurfaceVariant, kNestHatch); // stripes only: no outline
        shape.pattern = striped ? ShapePattern::Hatched : ShapePattern::Solid;
        b.put(shape);
      });
      if (piece.onPath && !ring.line && !piece.bridge.empty()) {
        part = 0;
        bandParts(ring.band, s0, s1, -half - ring.extension, -half, [&](Shape shape) {
          shape.id = sid(piece.node, SceneRole::Bridge, part++);
          shape.hittable = false;
          shape.fill = tone(p.primary, kNestBridge);
          b.put(shape);
        });
      }
      const bool thin = ring.thickness <= kNestThinRing;
      if (linked) {
        addSegmentEdge(b, ring, piece, s0, s1, opaque(p.tertiary), kNestFocusWidth);
      } else if (hot) {
        addSegmentEdge(b, ring, piece, s0, s1, opaque(p.onSurface), 1);
      } else if (piece.onPath && !thin) {
        addSegmentEdge(b, ring, piece, s0, s1, tone(p.primary, kNestPathEdge), kNestCoreEdgeWidth);
      }
      if (!merged && !thin) {
        addNestLabel(b, piece, (piece.s1 - piece.s0) * ring.thickness);
      }
    }

    // The linked tile's glow: a soft tertiary outline on its edge, fading over 8 px each way.
    void addGlow(Builder& b, const NestPiece& piece) {
      constexpr double kGlow = 0.9; // tertiary
      constexpr double kGlowSoftness = 8;
      const auto color = tone(b.palette.tertiary, kGlow);
      auto glow =
          ringAround(sid(piece.node, SceneRole::Mark), boxOf(piece.rect), 0, kNestTileRadius, color, kNestFocusWidth);
      glow.softness = static_cast<float>(kGlowSoftness);
      b.put(glow);
    }

    // The peek card's shadow: a soft copy of the card in the shadow color, dropped below it.
    void addCardShadow(Builder& b, const NestPeek& peek) {
      constexpr double kShadow = 0.55;
      constexpr double kShadowDrop = 10;
      constexpr double kShadowSoftness = 28;
      const auto& card = peek.card;
      auto layer =
          rectShape(sid(peek.node, SceneRole::Detail), card.x, card.y + kShadowDrop, card.w, card.h, kNestCardRadius);
      layer.fill = tone(b.palette.shadow, kShadow);
      layer.softness = static_cast<float>(kShadowSoftness);
      layer.hittable = false;
      b.put(layer);
    }

    // The keyboard cursor on a tile: a primary ring just outside it over a faint halo.
    void addCursor(Builder& b, const NestPiece& piece) {
      const auto box = boxOf(piece.rect);
      const auto id = sid(piece.node, SceneRole::Mark);
      const auto& primary = b.palette.primary;
      b.put(ringAround(id, box, 2, kNestTileRadius + 2, tone(primary, kNestCursorHalo), kNestCursorHaloWidth));
      b.put(ringAround(id, box, 1, kNestTileRadius + 1, opaque(primary), kNestCursorWidth));
    }

    SceneShapes buildNest(
        const Tree& tree, const NestScene& scene, std::span<const std::vector<NestPatch>> texture,
        const ShapeMarks& marks, const ColorPalette& palette
    ) {
      Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
      b.out.shapes.reserve(scene.pieces.size() * kShapesPerPiece);
      b.labels.reserve(scene.pieces.size());
      const auto focus = [&](NodeId id) { return id == kNoNode ? std::nullopt : nestFocus(tree, scene, id); };
      const auto linked = focus(marks.linked);
      const auto cursor = focus(marks.cursor);
      const bool textured = texture.size() == scene.pieces.size();
      // Rings first, so the core and its frame lie over the bridges.
      for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
        const auto& piece = scene.pieces[i];
        if (piece.kind != NestPieceKind::Tile && piece.kind != NestPieceKind::More && piece.ring < scene.rings.size()) {
          addSegment(b, scene, piece, linked == i);
        }
      }
      for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
        const auto& piece = scene.pieces[i];
        if (piece.kind == NestPieceKind::More) {
          addMoreTile(b, piece, linked == i);
        } else if (piece.kind == NestPieceKind::Tile) {
          addTile(b, piece, linked == i, false);
          if (textured) {
            addTexture(b, piece, texture[i]);
          }
        }
      }
      if (linked && *linked < scene.pieces.size()) {
        const auto kind = scene.pieces[*linked].kind;
        if (kind == NestPieceKind::Tile || kind == NestPieceKind::More) {
          addGlow(b, scene.pieces[*linked]);
        }
      }
      const auto core = boxOf(scene.core);
      const auto frame = sid(scene.folder, SceneRole::Detail);
      if (!scene.rings.empty()) {
        const auto color = tone(palette.primary, kNestCoreEdge);
        b.put(ringAround(frame, core, 0, scene.coreRadius, color, kNestCoreEdgeWidth));
      }
      if (scene.empty) {
        auto shape = ringAround(frame, core, 0, scene.coreRadius, opaque(palette.outlineVariant), kNestCoreEdgeWidth);
        shape.pattern = ShapePattern::DashedOutline;
        b.put(shape);
      }
      if (cursor && *cursor < scene.pieces.size()) {
        const auto& piece = scene.pieces[*cursor];
        if (piece.kind == NestPieceKind::Tile || piece.kind == NestPieceKind::More) {
          addCursor(b, piece);
        }
      }
      return b.finish();
    }
  } // namespace

  std::uint16_t shapeRole(SceneRole role, std::size_t part) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(role) << 8U) | std::min<std::size_t>(part, 0xFFU));
  }

  SceneRole sceneRole(std::uint16_t role) { return static_cast<SceneRole>(role >> 8U); }

  SceneShapes
  nestShapes(const Tree& tree, const NestScene& scene, const ShapeMarks& marks, const ColorPalette& palette) {
    return buildNest(tree, scene, {}, marks, palette);
  }

  SceneShapes nestShapes(
      const Tree& tree, const NestScene& scene, std::span<const std::vector<NestPatch>> texture,
      const ShapeMarks& marks, const ColorPalette& palette
  ) {
    return buildNest(tree, scene, texture, marks, palette);
  }

  SceneShapes
  nestPeekShapes(const Tree& tree, const NestPeek& peek, const ShapeMarks& marks, const ColorPalette& palette) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    const auto& card = peek.card;
    addCardShadow(b, peek);
    auto head = rectShape(sid(peek.node, SceneRole::Head), card.x, card.y, card.w, card.h, kNestCardRadius);
    head.fill = opaque(palette.surfaceContainerLow);
    head.stroke = b.hovered(head.id) ? opaque(palette.primary) : tone(palette.primary, kNestCardEdge);
    head.strokeWidth = 1;
    b.put(head);
    const double area = card.w * card.h;
    const auto headPiece =
        std::ranges::find_if(peek.pieces, [](const NestPiece& piece) { return piece.kind == NestPieceKind::PeekHead; });
    const auto accent = headPiece == peek.pieces.end() ? palette.onSurface : headPiece->color.accent;
    if (auto* name = b.label(
            peek.node, peek.name, peek.nameAt.x, peek.nameAt.y, peek.nameWidth, accent, area, {}, kPeekNameFont
        )) {
      name->weight = LabelWeight::Semibold;
    }
    const double factsFrom = peek.nameAt.x + peek.nameWidth + kPeekFactsGap;
    if (auto* facts = b.label(
            peek.node, peek.facts, factsFrom, peek.factsAt.y, peek.factsAt.x - factsFrom, palette.onSurfaceVariant,
            area, LabelAlign::End, kDetailFont
        )) {
      facts->fit = LabelFit::Whole;
    }
    for (const auto& piece : peek.pieces) {
      const bool linked = marked(marks.linked, piece.node, piece.nodes);
      if (piece.kind == NestPieceKind::PeekMore) {
        addMoreTile(b, piece, linked);
      } else if (piece.kind == NestPieceKind::PeekTile) {
        addTile(b, piece, linked, true);
      }
    }
    for (const auto& piece : peek.pieces) {
      if (piece.kind == NestPieceKind::PeekTile && marked(marks.cursor, piece.node)) {
        const auto ring = ringAround(
            sid(piece.node, SceneRole::Mark), boxOf(piece.rect), 1, kNestPeekTileRadius + 1, opaque(palette.primary),
            kNestCursorWidth
        );
        b.put(ring);
      }
    }
    return b.finish();
  }
} // namespace luciole::workbench
