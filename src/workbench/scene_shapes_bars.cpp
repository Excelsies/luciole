#include "workbench/format.h"
#include "workbench/measure.h"
#include "workbench/scene_shapes.h"
#include "workbench/scene_style.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Flame graph and ladder: the scenes drawn as bars.
namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kLabelPad = 6;
    constexpr double kBarInfoPad = 12;
    constexpr double kBarNameFont = 14;
    constexpr double kGrainInset = 2;
    constexpr double kGrainFrom = 12;  // narrowest merged piece with a grain
    constexpr double kGrainLeast = 10; // shortest
    // The current folder's icon on its bar, and the empty state over the strata ahead.
    constexpr double kFlameBarIconSize = 16;
    constexpr double kFlameBarIconLeft = 10; // from the bar's left edge
    constexpr double kFlameEmptyRadius = 6;
    constexpr double kFlameEmptyHintFrom = 40; // a box this tall has room for the second line
    constexpr double kFlameEmptyTitleAbove = 9;
    constexpr double kFlameEmptyHintBelow = 10;
    constexpr double kFlameEmptyHintFont = 12;
    constexpr double kFlameEmptyHint = 0.8; // onSurfaceVariant of the second line
    // Labels of the flame pieces: names in the first stratum ahead and elsewhere, and a value under its name.
    constexpr double kFlameFirstFont = 13;
    constexpr double kFlameFont = 12;
    constexpr double kFlameNameAbove = 7;  // the name's middle above the piece's middle
    constexpr double kFlameValueBelow = 8; // the value's middle below it
    constexpr double kFlameValueInk = 0.8; // of the name's ink

    // A flame item under the focused one: on its chain up, in its subtree below, or off it in the strata ahead.
    enum class Lit : std::uint8_t { Plain, Chain, Subtree, Dim };

    struct FlameSpot {
      std::size_t row = 0;
      std::size_t item = 0;
    };

    bool aheadRow(const FlameScene& scene, std::size_t row) {
      return row < scene.rows.size() && scene.rows[row].row.kind == FlameRowKind::Ahead;
    }

    ShapeId flameId(const FlameItem& item) {
      const auto role = item.bar ? SceneRole::Path
          : item.far             ? SceneRole::Far
          : item.more            ? SceneRole::More
                                 : SceneRole::Piece;
      return sid(item.more ? item.owner : item.node, role);
    }

    // The hovered item, else the one standing for the linked node, the strata ahead first.
    std::optional<FlameSpot> flameFocusSpot(const Builder& b, const FlameScene& scene) {
      std::optional<FlameSpot> linked;
      for (std::size_t r = 0; r < scene.rows.size(); ++r) {
        const auto& items = scene.rows[r].items;
        for (std::size_t i = 0; i < items.size(); ++i) {
          if (items[i].bar) {
            continue;
          }
          if (b.hovered(flameId(items[i]))) {
            return FlameSpot{.row = r, .item = i};
          }
          const bool better = !linked || (!aheadRow(scene, linked->row) && aheadRow(scene, r));
          if (better && marked(b.marks.linked, items[i].node, items[i].list)) {
            linked = FlameSpot{.row = r, .item = i};
          }
        }
      }
      return linked;
    }

    std::vector<std::vector<Lit>> flameLit(const FlameScene& scene, std::optional<FlameSpot> focus) {
      std::vector<std::vector<Lit>> lit;
      lit.reserve(scene.rows.size());
      for (const auto& row : scene.rows) {
        lit.emplace_back(row.items.size(), Lit::Plain);
      }
      if (!focus) {
        return lit;
      }
      const auto [row, item] = *focus;
      lit[row][item] = Lit::Chain;
      if (scene.rows[row].items[item].more || !aheadRow(scene, row)) {
        return lit;
      }
      // Up through the strata ahead: each row is visited once.
      std::size_t index = item;
      for (std::size_t r = row; r > 0 && aheadRow(scene, r - 1); --r) {
        index = scene.rows[r].items[index].parent;
        if (index >= lit[r - 1].size()) {
          break;
        }
        lit[r - 1][index] = Lit::Chain;
      }
      for (std::size_t r = row + 1; aheadRow(scene, r); ++r) {
        const auto& items = scene.rows[r].items;
        for (std::size_t i = 0; i < items.size(); ++i) {
          const auto parent = items[i].parent;
          const bool under =
              parent < lit[r - 1].size() && ((r - 1 == row && parent == item) || lit[r - 1][parent] == Lit::Subtree);
          lit[r][i] = under ? Lit::Subtree : Lit::Plain;
        }
      }
      for (std::size_t r = 0; r < scene.rows.size(); ++r) {
        for (auto& state : lit[r]) {
          state = aheadRow(scene, r) && state == Lit::Plain ? Lit::Dim : state;
        }
      }
      return lit;
    }

    // Fine vertical cuts across a merged piece, so it reads as many small folders.
    void addGrain(Builder& b, const FlameItem& item, const FlameBox& box, double y, double h, float opacity) {
      if (box.w <= kGrainFrom || h < kGrainLeast) {
        return;
      }
      const auto color = opaque(b.palette.surfaceContainerLow);
      std::size_t k = 0;
      // One cut per kFlameGrainStep px of width.
      for (double x = box.x + kFlameGrainStep; x < box.x + box.w - kGrainInset; x += kFlameGrainStep) {
        b.line(
            sid(item.owner, SceneRole::Detail, k++), {.x = x + 0.5, .y = y + kGrainInset},
            {.x = x + 0.5, .y = y + h - kGrainInset}, color, 1
        );
        b.out.shapes.back().opacity = opacity;
      }
    }

    void addFlameBar(
        Builder& b, const FlameItem& item, const FlameBox& box, double y, double h, WorkbenchMeasure by, Rgb accent
    ) {
      const auto& p = b.palette;
      auto shape = rectShape(flameId(item), box.x, y, box.w, h, kFlameBarRadius);
      shape.fill = opaque(b.hovered(shape.id) ? p.surfaceContainerHighest : p.surfaceContainerHigh);
      shape.pattern = hatched(b.tree, item.node) ? ShapePattern::Hatched : ShapePattern::Solid;
      shape.stroke = tone(p.onSurfaceVariant, kHatch);
      b.put(shape);
      if (!hasNode(b.tree, item.node)) {
        return;
      }
      const double middle = y + h / 2;
      const double half = box.w / 2;
      auto measure = measureLabel(b.tree, item.node, by);
      const auto items = itemsLabel(b.tree, item.node);
      // Under Size by Items the measure is the count: told once.
      const auto info = measure == items ? measure : measure + std::string(kFactsDot) + items;
      const double area = box.w * h;
      if (auto* facts = b.label(
              item.node, info, box.x + half, middle, half - kBarInfoPad, p.onSurfaceVariant, area, LabelAlign::End,
              kCaptionFont
          )) {
        facts->fit = LabelFit::Whole;
        facts->shortText = std::move(measure);
      }
      if (auto* name = b.label(
              item.node, nodeName(b.tree, item.node), box.x + kFlameBarIcon, middle, half - kFlameBarIcon, p.onSurface,
              area, LabelAlign::Start, kBarNameFont
          )) {
        // The folder's icon in the room before its name.
        name->icon = {
            .category = static_cast<FileCategory>(b.tree.nodes[item.node].category),
            .size = static_cast<float>(kFlameBarIconSize),
            .left = static_cast<float>(kFlameBarIconLeft - kFlameBarIcon),
            .top = static_cast<float>(middle - kFlameBarIconSize / 2) - name->y,
            .color = opaque(accent),
        };
      }
    }

    // A dashed outline over the strata ahead of a folder with no folders, and, when it holds files, a caption that
    // sends to the list. A folder with no visible entries gets the view's own note in the box instead.
    void addFlameEmpty(Builder& b, const Rect& box, bool showHidden) {
      const auto& p = b.palette;
      const NodeId folder = b.tree.root;
      auto outline = rectShape(
          sid(folder, SceneRole::Detail), box.x + 0.5, box.y + 0.5, box.width - 1, box.height - 1, kFlameEmptyRadius
      );
      outline.fill.a = 0;
      outline.stroke = opaque(p.outlineVariant);
      outline.strokeWidth = 1;
      outline.pattern = ShapePattern::DashedOutline;
      outline.hittable = false;
      b.put(outline);
      const bool files = std::ranges::any_of(children(b.tree, folder), [&](NodeId id) {
        return hasNode(b.tree, id) && visibleEntry(b.tree.nodes[id], showHidden);
      });
      if (!files) {
        return;
      }
      const double middle = box.y + box.height / 2;
      const bool hint = box.height >= kFlameEmptyHintFrom;
      const double area = box.width * box.height;
      b.label(
          folder, "No folders inside", box.x, hint ? middle - kFlameEmptyTitleAbove : middle, box.width,
          p.onSurfaceVariant, area, LabelAlign::Center, kCaptionFont
      );
      if (!hint) {
        return;
      }
      if (auto* line = b.label(
              folder, "The files are in the list below", box.x, middle + kFlameEmptyHintBelow, box.width,
              p.onSurfaceVariant, area, LabelAlign::Center, kFlameEmptyHintFont
          )) {
        line->color = tone(p.onSurfaceVariant, kFlameEmptyHint);
      }
    }

    // A flame piece's label: "+N" centered on a merged piece, else the name, with the value whole under it or at the
    // right end when the scene gave it one.
    void
    addFlameLabel(Builder& b, const FlameItem& item, const Rect& box, Rgb ink, double opacity, WorkbenchMeasure by) {
      const double inside = box.width - 2 * kLabelPad;
      const double area = box.width * box.height;
      const double middle = box.y + box.height / 2;
      const double left = box.x + kLabelPad;
      const double size = item.style == FlameLabelStyle::FirstAhead ? kFlameFirstFont : kFlameFont;
      if (item.more) {
        if (auto* count = b.label(
                item.owner, std::format("+{}", item.list.size()), left, middle, inside, ink, area, LabelAlign::Center,
                size
            )) {
          count->color = tone(ink, opacity);
          count->fit = LabelFit::Whole;
        }
        return;
      }
      const bool stacked = item.label == FlameLabel::StackedNameValue;
      const bool beside = item.label == FlameLabel::NameValue;
      const double nameRoom = beside ? inside - item.valueWidth - kFlameValueGap : inside;
      if (auto* name = b.label(
              item.node, nodeName(b.tree, item.node), left, stacked ? middle - kFlameNameAbove : middle, nameRoom, ink,
              area, LabelAlign::Start, size
          )) {
        name->color = tone(ink, opacity);
      }
      if (!stacked && !beside) {
        return;
      }
      if (auto* value = b.label(
              item.node, measureLabel(b.tree, item.node, by), left, stacked ? middle + kFlameValueBelow : middle,
              inside, ink, area, stacked ? LabelAlign::Start : LabelAlign::End, kDetailFont
          )) {
        value->color = tone(ink, opacity * kFlameValueInk);
        value->fit = LabelFit::Whole;
      }
    }

    // The inner corner of a bar's first or last piece, kept square under its rounded corners.
    void addFiller(Builder& b, const Shape& piece, bool right) {
      const auto& r = piece.rect;
      const double width = std::min<double>(kLadderRadius, r.width / 2);
      const double x = right ? r.x + r.width - width : r.x;
      auto filler = rectShape(sid(piece.id.node, SceneRole::Detail, 1), x, r.y, width, r.height);
      filler.fill = piece.fill;
      filler.hittable = false;
      b.put(filler);
    }

    // A ladder piece: rounded at the bar's ends over a square filler, so its own outline and hatch stay on top, and
    // parted from the next piece by a line of surface color unless it is outlined. Every part is opaque, so where
    // they overlap nothing shows through.
    void addLadderPiece(Builder& b, Shape shape, std::size_t index, std::size_t count, Rgb seamColor) {
      const bool first = index == 0;
      const bool last = index + 1 == count;
      shape.rect.radius = first || last ? static_cast<float>(kLadderRadius) : 0.0F;
      shape.kind = first || last ? ShapeKind::RoundedRect : ShapeKind::Rect;
      if (first != last) {
        addFiller(b, shape, first);
      }
      b.put(shape);
      if (!last && shape.strokeWidth == 0) {
        const auto& r = shape.rect;
        auto seam = rectShape(sid(shape.id.node, SceneRole::Detail), r.x + r.width - 1, r.y, 1, r.height);
        seam.fill = opaque(seamColor);
        seam.hittable = false;
        b.put(seam);
      }
    }

    // A ladder piece on the way to the current folder, outlined `width` wide just outside its edges.
    struct PathOutline {
      double x = 0;
      double y = 0;
      double w = 0;
      double h = 0;
      double width = 0;
      bool roundLeft = false; // the piece starts its bar
      bool roundRight = false;
    };

    // Straight sides as lines, and at a bar's end the corners as quarter rings around the piece's own rounded
    // corners, so the outline follows the bar's shape there and stays square against the neighbors.
    void addPathOutline(Builder& b, NodeId node, const PathOutline& o, Rgba8 color) {
      const double c = o.width / 2; // the outline's middle lies this far outside the piece
      const double r = std::min({kLadderRadius, o.w / 2, o.h / 2});
      const double left = o.x - c;
      const double right = o.x + o.w + c;
      const double top = o.y - c;
      const double bottom = o.y + o.h + c;
      // A square corner is covered by the top and bottom lines reaching past the sides.
      const double fromX = o.roundLeft ? o.x + r : left - c;
      const double toX = o.roundRight ? o.x + o.w - r : right + c;
      std::size_t part = 0;
      const auto line = [&](Point from, Point to) {
        if (from.x < to.x || from.y < to.y) {
          b.line(sid(node, SceneRole::Mark, part++), from, to, color, o.width);
        }
      };
      line({.x = fromX, .y = top}, {.x = toX, .y = top});
      line({.x = fromX, .y = bottom}, {.x = toX, .y = bottom});
      line({.x = left, .y = o.roundLeft ? o.y + r : top}, {.x = left, .y = o.roundLeft ? o.y + o.h - r : bottom});
      line({.x = right, .y = o.roundRight ? o.y + r : top}, {.x = right, .y = o.roundRight ? o.y + o.h - r : bottom});
      const auto corner = [&](double cx, double cy, double from, double to) {
        auto arc = sectorShape(sid(node, SceneRole::Mark, part++), {.x = cx, .y = cy}, r, r + o.width, from, to);
        arc.fill = color;
        arc.hittable = false;
        b.put(arc);
      };
      if (o.roundLeft) {
        corner(o.x + r, o.y + r, 270, 360);
        corner(o.x + r, o.y + o.h - r, 180, 270);
      }
      if (o.roundRight) {
        corner(o.x + o.w - r, o.y + r, 0, 90);
        corner(o.x + o.w - r, o.y + o.h - r, 90, 180);
      }
    }
  } // namespace

  SceneShapes flameShapes(
      const Tree& tree, const LayoutSettings& settings, const FlameScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette
  ) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    Colors colors{.tree = tree, .settings = settings, .palette = palette, .ranges = {}};
    const auto lit = flameLit(scene, flameFocusSpot(b, scene));
    for (std::size_t r = 0; r < scene.rows.size(); ++r) {
      const auto& row = scene.rows[r];
      const double y = row.row.y;
      const double h = row.row.h;
      const bool firstAhead = row.row.kind == FlameRowKind::Ahead && row.row.level == 1;
      for (std::size_t i = 0; i < row.items.size(); ++i) {
        const auto& item = row.items[i];
        const auto box = flameBox(item.x, item.w, h, item.bar);
        if (item.bar) {
          addFlameBar(b, item, box, y, h, settings.sizeBy, colors.of(item.node).accent);
          continue;
        }
        const auto color = colors.of(item.node);
        const Rgb accent = item.more ? palette.secondary : color.accent;
        Rgb fill = item.more  ? palette.surfaceContainerHighest
            : item.accentFill ? mix(palette.surface, color.accent, kFlameAccentFill)
                              : mix(palette.surface, color.fill, item.strength);
        const auto state = lit[r][i];
        fill = state == Lit::Chain ? accent : state == Lit::Subtree ? mix(fill, accent, kFlameSubTint) : fill;
        auto shape = rectShape(flameId(item), box.x, y, box.w, h, box.r);
        const bool cursor = firstAhead && marked(marks.cursor, item.node, item.list);
        const bool hot = b.hovered(shape.id);
        const bool pin = item.pinned && h >= kGrainLeast;
        shape.fill = opaque(fill);
        shape.stroke = hot ? opaque(palette.onSurface)
            : pin          ? opaque(palette.primary)
                           : tone(palette.onSurfaceVariant, kHatch);
        shape.strokeWidth = static_cast<float>(hot ? kFlameHotWidth : pin ? kFlamePinWidth : 0.0);
        shape.hatch = tone(palette.onSurfaceVariant, kHatch);
        shape.opacity = state == Lit::Dim ? static_cast<float>(kFlameDim) : 1.0F;
        shape.pattern = hatched(tree, item.node) ? ShapePattern::Hatched : ShapePattern::Solid;
        b.put(shape);
        if (item.more) {
          addGrain(b, item, box, y, h, shape.opacity);
        }
        if (cursor) {
          // The keyboard cursor stays at full strength over a dimmed piece.
          b.put(ringAround(
              sid(item.node, SceneRole::Mark), shape.rect, -kFlameCursorWidth / 2, box.r - kFlameCursorWidth / 2,
              opaque(palette.primary), kFlameCursorWidth
          ));
        }
        if (item.label != FlameLabel::None) {
          const auto ink = item.more && state != Lit::Chain ? palette.onSurfaceVariant : inkFor(fill, palette);
          const Rect drawn{.x = box.x, .y = y, .width = box.w, .height = h};
          addFlameLabel(b, item, drawn, ink, shape.opacity, settings.sizeBy);
        }
      }
    }
    if (scene.empty && scene.aheadBox) {
      addFlameEmpty(b, *scene.aheadBox, settings.showHidden);
    }
    return b.finish();
  }

  SceneShapes ladderShapes(
      const Tree& tree, const LayoutSettings& settings, const LadderScene& scene, const ShapeMarks& marks,
      const ColorPalette& palette
  ) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    Colors colors{.tree = tree, .settings = settings, .palette = palette, .ranges = {}};
    for (const auto& row : scene.rows) {
      const double middle = row.y + row.h / 2;
      const auto faded = [&](Rgb color) { return mix(color, palette.surfaceContainerLow, 1 - row.fade); };
      const std::size_t count = row.pieces.size() + (row.rest ? 1 : 0);
      for (std::size_t i = 0; i < row.pieces.size(); ++i) {
        const auto& piece = row.pieces[i];
        auto shape = rectShape(sid(piece.node, SceneRole::Piece), piece.x, row.y, piece.w, row.h);
        const bool linked = marked(marks.linked, piece.node);
        const bool hot = b.hovered(shape.id);
        Rgb fill = mix(colors.of(piece.node).accent, palette.surface, kLadderFillToSurface);
        fill = linked ? mix(fill, palette.onSurface, kLadderFocusTint)
            : hot     ? mix(fill, palette.onSurface, kLadderHotTint)
                      : fill;
        shape.fill = opaque(faded(fill));
        shape.stroke = linked ? opaque(faded(palette.onSurface)) : tone(faded(palette.onSurfaceVariant), kHatch);
        shape.strokeWidth = linked ? static_cast<float>(kLadderFocusWidth) : 0.0F;
        shape.hatch = tone(faded(palette.onSurfaceVariant), kHatch);
        shape.pattern = hatched(tree, piece.node) ? ShapePattern::Hatched : ShapePattern::Solid;
        addLadderPiece(b, shape, i, count, faded(palette.surface));
        if (piece.labeled) {
          b.label(
              piece.node, nodeName(tree, piece.node), piece.x + kLabelPad, middle, piece.w - 2 * kLabelPad,
              faded(inkFor(fill, palette)), piece.w * row.h, LabelAlign::Start, kDetailFont
          );
        }
      }
      if (const auto& rest = row.rest) {
        auto shape = rectShape(sid(row.folder, SceneRole::More), rest->x, row.y, rest->w, row.h);
        shape.fill = opaque(faded(palette.surfaceContainerHigh));
        shape.stroke = opaque(faded(palette.surfaceContainerHighest)); // the stripes
        shape.pattern = ShapePattern::Hatched;
        addLadderPiece(b, shape, count - 1, count, faded(palette.surface));
        if (row.restLabeled) {
          if (auto* label = b.label(
                  row.folder, std::format("+{}", rest->count), rest->x, middle, rest->w,
                  faded(palette.onSurfaceVariant), rest->w * row.h, LabelAlign::Center, kDetailFont
              )) {
            label->fit = LabelFit::Whole;
          }
        }
      }
      // The way to the current folder: an outline over its neighbors, thinner on far bars.
      for (std::size_t i = 0; i < row.pieces.size(); ++i) {
        const auto& piece = row.pieces[i];
        if (piece.onPath) {
          const double width = row.index < kLadderWidePathBars ? kLadderPathWidth : kLadderFarPathWidth;
          const PathOutline outline{
              .x = piece.x,
              .y = row.y,
              .w = piece.w,
              .h = row.h,
              .width = width,
              .roundLeft = i == 0,
              .roundRight = i + 1 == count,
          };
          addPathOutline(b, piece.node, outline, opaque(faded(palette.primary)));
        }
      }
      if (row.named) {
        const double right = row.x - (kLadderGutter - kLadderNameRoom) / 2;
        if (auto* label = b.label(
                row.folder, nodeName(tree, row.folder), right - kLadderNameRoom, middle, kLadderNameRoom,
                palette.onSurfaceVariant, row.w * row.h, LabelAlign::End, kDetailFont
            )) {
          label->color = tone(palette.onSurfaceVariant, kLadderName);
        }
      }
    }
    return b.finish();
  }
} // namespace luciole::workbench
