#include "workbench/flame_row.h"
#include "workbench/flow_topology.h"
#include "workbench/nest_labels.h"
#include "workbench/scene_shapes.h"
#include "workbench/scene_style.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The flow diagram: links, ribbons, record bars, ancestor pills, labels and the note of a folder with no folders.
namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kFlowBarPad = 4; // flow hit areas, as flowHitAt measures them
    constexpr double kFlowBarHit = 18;
    constexpr double kFlowStemHit = 16;
    constexpr double kFlowLabelPad = 6; // a label answers from this far before its text to 2 px past it
    constexpr double kFontBase = 12;    // the size `font` holds advance widths for
    constexpr double kRingGrow = 0.75;
    // The note: its center right of the current bar, a dashed rule leading to it, two lines under the rule.
    constexpr double kNoteShift = 40;
    constexpr double kNoteRuleFrom = 150;
    constexpr double kNoteRuleTo = 110;
    constexpr double kNoteTitleBelow = 8;
    constexpr double kNoteHintBelow = 26;
    constexpr double kNoteWidth = 260;
    constexpr double kNoteIcon = 24;
    constexpr double kNoteIconAbove = 38;  // from the middle of the current bar to the icon's top
    constexpr double kNoteIconTone = 0.45; // onSurfaceVariant

    struct FlowNote {
      std::string_view title;
      std::string_view hint;
    };

    FlowNote flowNote(FlowEmpty empty) {
      switch (empty) {
      case FlowEmpty::Empty:
        return {.title = "This folder is empty", .hint = "There is nothing to chart"};
      case FlowEmpty::NoFolders:
        return {.title = "No subfolders here", .hint = "Its files are in the list below"};
      case FlowEmpty::NotExpanded:
        return {.title = "Not expanded in this snapshot", .hint = "Its totals are known, its contents are not"};
      case FlowEmpty::Unreadable:
        return {.title = "This folder could not be read", .hint = "The list shows what is known"};
      case FlowEmpty::NotMeasured:
        return {.title = "Not measured here", .hint = "This location is not a local folder."};
      case FlowEmpty::None:
        break;
      }
      return {};
    }

    ShapeRect boxOf(double x, double y, double width, double height) {
      const auto f = [](double value) { return static_cast<float>(value); };
      return {.x = f(x), .y = f(y), .width = f(width), .height = f(height), .radius = 0};
    }

    // A record's bar: folders solid with an accent stripe at the left, More dashed, files hatched and outlined.
    void addRecordBar(Builder& b, const FlowRecord& record, ShapeId id, const NodeColor& color, float opacity) {
      const auto& p = b.palette;
      auto bar = rectShape(id, record.x, record.y, record.width, record.height, kFlowRadius);
      bar.opacity = opacity;
      const bool quiet = record.kind == FlowRecordKind::More || record.kind == FlowRecordKind::Files;
      bar.fill = opaque(quiet ? p.surfaceContainerHigh : color.fill);
      bar.stroke = tone(p.onSurfaceVariant, record.kind == FlowRecordKind::Files ? kFlowFilesHatch : kHatch);
      bar.pattern = record.kind == FlowRecordKind::Files || hatched(b.tree, record.node) ? ShapePattern::Hatched
                                                                                         : ShapePattern::Solid;
      if (record.kind == FlowRecordKind::More) {
        bar.stroke = tone(p.onSurfaceVariant, kFlowQuietEdge);
        bar.strokeWidth = 1;
        bar.pattern = ShapePattern::DashedOutline;
      }
      b.put(bar);
      if (record.kind == FlowRecordKind::Files) {
        auto edge = bar;
        edge.id = sid(record.node, SceneRole::Detail, record.column);
        edge.fill.a = 0;
        edge.stroke = tone(p.onSurfaceVariant, kFlowQuietEdge);
        edge.strokeWidth = 1;
        edge.pattern = ShapePattern::Solid;
        edge.hittable = false;
        b.put(edge);
      } else if (!quiet && record.height > 2 * kFlowStripeInset) {
        auto stripe = rectShape(
            sid(record.node, SceneRole::Detail), record.x, record.y + kFlowStripeInset, kFlowStripeWidth,
            record.height - 2 * kFlowStripeInset
        );
        stripe.fill = tone(color.accent, kFlowStripe);
        stripe.opacity = opacity;
        stripe.hittable = false;
        b.put(stripe);
      }
      const double height = std::max(record.height, kFlowBarHit);
      const double top = record.y + record.height / 2 - height / 2;
      b.put(hitArea(id, record.x - kFlowBarPad, top, record.width + 2 * kFlowBarPad, height));
    }

    void addNote(Builder& b, const FlowLayout& layout) {
      const auto note = flowNote(layout.topology.empty);
      if (note.title.empty() || layout.topology.records.empty()) {
        return;
      }
      const auto& p = b.palette;
      const auto& current = layout.topology.records.front();
      const double cx = current.x + (layout.width - current.x) / 2 + kNoteShift;
      const double cy = current.y + current.height / 2;
      const double from = current.x + current.width + kNoteRuleFrom;
      const double to = cx - kNoteRuleTo;
      if (to > from) {
        auto rule = rectShape(sid(layout.topology.folder, SceneRole::Detail), from, cy, to - from, 1);
        rule.fill.a = 0;
        rule.stroke = tone(p.outlineVariant, kFlowNoteRule);
        rule.strokeWidth = 1;
        rule.pattern = ShapePattern::DashedOutline;
        rule.hittable = false;
        b.put(rule);
      }
      const auto folder = layout.topology.folder;
      const double x = cx - kNoteWidth / 2;
      const double area = kNoteWidth * kCaptionFont;
      if (auto* title = b.label(
              folder, std::string(note.title), x, cy + kNoteTitleBelow, kNoteWidth, p.onSurfaceVariant, area,
              LabelAlign::Center, kCaptionFont
          )) {
        // The folder's icon, faint, centered over the title.
        title->icon = {
            .category = FileCategory::Folder,
            .size = static_cast<float>(kNoteIcon),
            .left = static_cast<float>((kNoteWidth - kNoteIcon) / 2),
            .top = static_cast<float>(cy - kNoteIconAbove) - title->y,
            .color = tone(p.onSurfaceVariant, kNoteIconTone),
        };
      }
      if (auto* hint = b.label(
              folder, std::string(note.hint), x, cy + kNoteHintBelow, kNoteWidth, p.onSurfaceVariant, area,
              LabelAlign::Center, kDetailFont
          )) {
        hint->color = tone(p.onSurfaceVariant, kFlowNoteHint);
      }
    }
  } // namespace

  SceneShapes flowShapes(
      const Tree& tree, const LayoutSettings& settings, const FlowLayout& layout, const ShapeMarks& marks,
      const ColorPalette& palette, const LabelFont& font
  ) {
    Builder b{.tree = tree, .marks = marks, .palette = palette, .out = {}, .labels = {}};
    Colors colors{.tree = tree, .settings = settings, .palette = palette, .ranges = {}};
    const auto& topology = layout.topology;
    const auto& records = topology.records;
    const auto idOf = [&records](std::size_t i, bool ribbon) {
      const auto& r = records[i];
      const bool more = r.kind == FlowRecordKind::More;
      if (ribbon || r.kind == FlowRecordKind::Files) {
        return ribbon ? sid(r.node, more ? SceneRole::MoreRibbon : SceneRole::Ribbon)
                      : sid(r.node, SceneRole::Files, r.column);
      }
      return sid(r.node, more ? SceneRole::More : SceneRole::Piece);
    };
    const auto recordOf = [&](NodeId id) { return id == kNoNode ? std::nullopt : flowRecordFor(tree, topology, id); };
    std::optional<std::size_t> hot;
    for (std::size_t i = 0; i < records.size() && marks.hovered; ++i) {
      if (b.hovered(idOf(i, false))) {
        hot = i;
        break;
      }
    }
    const auto linked = recordOf(marks.linked);
    const auto focus = hot ? hot : linked;
    const auto lit = focus ? flowFocus(topology, *focus) : std::vector<bool>{};
    const bool focused = !lit.empty();
    const auto on = [&](std::size_t i) { return !focused || (i < lit.size() && lit[i]); };
    for (std::size_t i = 0; i < layout.links.size(); ++i) {
      const auto& link = layout.links[i];
      b.line(
          sid(topology.folder, SceneRole::Link, i), {.x = link.x0, .y = link.centerY},
          {.x = link.x1, .y = link.centerY}, opaque(palette.outlineVariant), kFlowLinkWidth
      );
      b.out.shapes.back().opacity = static_cast<float>(focused ? kFlowLinkDim : 1.0);
    }
    // Off the lit path first, so the lit ribbons and bars lie on top.
    for (const bool pass : {false, true}) {
      for (const auto& ribbon : layout.ribbons) {
        if (ribbon.record >= records.size() || on(ribbon.record) != pass) {
          continue;
        }
        Shape shape;
        shape.id = idOf(ribbon.record, true);
        shape.kind = ShapeKind::Ribbon;
        for (std::size_t k = 0; k < 4; ++k) {
          const auto& top = ribbon.top[k];
          const auto& bottom = ribbon.bottom[3 - k]; // the scene runs the bottom edge right to left
          shape.ribbon.top[k] = {.x = static_cast<float>(top.x), .y = static_cast<float>(top.y)};
          shape.ribbon.bottom[k] = {.x = static_cast<float>(bottom.x), .y = static_cast<float>(bottom.y)};
        }
        const auto color = colors.of(records[ribbon.record].node);
        shape.fill = opaque(ribbon.muted ? palette.onSurfaceVariant : mix(color.fill, color.accent, kFlowRibbonTint));
        const double base = ribbon.muted ? kFlowRibbonMuted : kFlowRibbon;
        shape.opacity = static_cast<float>(focused ? (pass ? kFlowRibbonLit : kFlowRibbonDim) : base);
        shape.hittable = false;
        b.put(shape);
      }
    }
    const auto cursor = recordOf(marks.cursor);
    const auto pillOpacity = static_cast<float>(focused ? kFlowDim : 1.0);
    const auto fits = [&font](std::string_view text) { return labelTextWidth(text, font); };
    for (const auto& pill : layout.stem) {
      // A pill with a name shows it whole or cut; when no cut fits, the pill is left out, never drawn empty.
      const bool named = pill.labeled && pill.textWidth > 0;
      auto name = named ? fitNestText(nodeName(tree, pill.node), pill.textWidth, fits) : std::string{};
      if (named && name.empty()) {
        continue;
      }
      const auto id = sid(pill.node, SceneRole::Stem);
      const double cy = pill.y + pill.height / 2;
      const auto color = colors.of(pill.node);
      const bool hotPill = b.hovered(id);
      const double radius = std::min(pill.width, pill.height) / 2;
      auto shape = pill.kind == FlowPillKind::Dot ? circleShape(id, {.x = pill.x + pill.width / 2, .y = cy}, radius)
                                                  : rectShape(id, pill.x, pill.y, pill.width, pill.height, radius);
      shape.fill = opaque(hotPill ? color.accent : color.fill);
      shape.opacity = static_cast<float>(pill.alpha) * pillOpacity;
      b.put(shape);
      if (hotPill) {
        const auto box = boxOf(pill.x, pill.y, pill.width, pill.height);
        b.put(ringAround(
            sid(pill.node, SceneRole::Mark), box, kRingGrow, radius + kRingGrow, opaque(palette.onSurface),
            kFlowRingWidth
        ));
      }
      const double height = std::max(pill.height, kFlowStemHit);
      b.put(hitArea(id, pill.x - 1, cy - height / 2, pill.width + 2, height));
      if (named) {
        const double x = pill.x + pill.width / 2 - pill.textWidth / 2;
        const auto ink = hotPill ? inkFor(color.accent, palette) : color.ink;
        b.label(pill.node, std::move(name), x, cy, pill.textWidth, ink, pill.width * pill.height, LabelAlign::Center);
      }
    }
    for (const bool pass : {false, true}) {
      for (std::size_t i = 0; i < records.size(); ++i) {
        if (on(i) != pass) {
          continue;
        }
        const auto& r = records[i];
        const auto color = colors.of(r.node);
        addRecordBar(b, r, idOf(i, false), color, static_cast<float>(on(i) ? 1.0 : kFlowDim));
        const auto box = boxOf(r.x, r.y, r.width, r.height);
        const auto mark = sid(r.node, SceneRole::Mark, r.column);
        if (hot == i || linked == i) {
          b.put(ringAround(mark, box, kRingGrow, kFlowRadius + kRingGrow, opaque(palette.onSurface), kFlowRingWidth));
        } else if (focused && on(i) && r.kind == FlowRecordKind::Folder) {
          b.put(ringAround(mark, box, kRingGrow, kFlowRadius + kRingGrow, opaque(color.accent), kFlowRingWidth));
        }
        if (cursor == i && r.kind == FlowRecordKind::Folder && r.node == marks.cursor) {
          const auto ink = opaque(palette.primary);
          b.put(ringAround(mark, box, kFlowCursorGrow, kFlowRadius + 1, ink, kFlowCursorWidth));
        }
      }
    }
    for (const auto& label : layout.labels) {
      if (label.record >= records.size()) {
        continue;
      }
      const auto& r = records[label.record];
      const double area = r.width * r.height;
      const double shift = label.lines == 2 ? label.height / 4 : 0;
      const bool quiet = r.kind == FlowRecordKind::More || r.kind == FlowRecordKind::Files;
      const double alpha = on(label.record) ? 1.0 : kFlowLabelDim;
      const auto nameInk = quiet ? palette.onSurfaceVariant : palette.onSurface;
      auto nameText = flowRecordName(tree, topology, label.record);
      double drawn = std::min(labelTextWidth(nameText, font), label.maxWidth);
      auto* name = b.label(r.node, std::move(nameText), label.x, label.centerY - shift, label.maxWidth, nameInk, area);
      if (name == nullptr) {
        continue;
      }
      name->color = tone(nameInk, alpha);
      if (r.kind == FlowRecordKind::More) {
        name->fit = LabelFit::Whole; // "+N more" or "N folders" is a count
      }
      if (label.lines == 2) {
        auto text = flowRecordValue(tree, topology, label.record, settings.sizeBy);
        const double width = labelTextWidth(text, font) * kDetailFont / kFontBase;
        const auto ink = palette.onSurfaceVariant;
        if (auto* value = b.label(
                r.node, std::move(text), label.x, label.centerY + shift, label.maxWidth, ink, area, {}, kDetailFont
            )) {
          value->color = tone(ink, alpha);
          value->fit = LabelFit::Whole;
          drawn = width <= label.maxWidth ? std::max(drawn, width) : drawn; // a value too wide is left out
        }
      }
      // The label stands for its record, over the ribbons it may lie on.
      b.put(hitArea(
          idOf(label.record, false), label.x - kFlowLabelPad, label.centerY - label.height / 2,
          drawn + kFlowLabelPad + 2, label.height
      ));
    }
    addNote(b, layout);
    return b.finish();
  }
} // namespace luciole::workbench
