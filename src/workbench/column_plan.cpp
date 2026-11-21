#include "workbench/column_plan.h"

#include <algorithm>
#include <cmath>

namespace luciole::workbench {
  namespace {
    // Value within lo..hi; when hi < lo the minimum wins.
    double clampLow(double value, double lo, double hi) { return std::max(lo, std::min(hi, value)); }

    // Every width rounded here is positive or clamped to a positive minimum, so halves away from zero are halves up.
    double roundWidth(double value) { return std::round(value); }

    bool validColumnWidth(double width) { return std::isfinite(width) && width >= kColumnMin && width <= kColumnMax; }

    double validListMin(double listMin) { return std::isfinite(listMin) && listMin > 0 ? listMin : kListFloor; }

    // A scale that is not finite and positive reads as 1.
    double validScale(double scale) { return std::isfinite(scale) && scale > 0 ? scale : 1.0; }

    // Widths of the bands at a text scale, in whole pixels; the same as the constants at 1.
    double bandWidth(std::size_t level, double scale) {
      return std::round(kAncestorStripWidths[std::min(level - 1, kAncestorStripWidths.size() - 1)] * scale);
    }

    double stackWidth(double scale) { return std::round(kStackStripWidth * scale); }

    // The `count` nearest named bands with the gap after each.
    double bandsWidth(std::size_t count, double scale) {
      double width = 0;
      for (std::size_t level = 1; level <= count; ++level) { // at most kMaxStrips passes
        width += bandWidth(level, scale) + kStripGap;
      }
      return width;
    }

    double stripsMin(double scale) { return bandsWidth(1, scale) + stackWidth(scale) + kStripGap; }
    double stripsDefault(double scale) { return bandsWidth(4, scale) + stackWidth(scale) + kStripGap; }
    double stripsMax(double scale) { return bandsWidth(12, scale) + stackWidth(scale) + kStripGap; }

    // `named` ancestor strips, nearest last, behind one stack strip for the levels beyond them; the nearest ends at
    // `right`.
    std::vector<Strip> buildStrips(std::size_t named, std::size_t count, double right, double scale) {
      std::vector<Strip> strips;
      strips.reserve(named + 1);
      double x = right - bandsWidth(named, scale);
      if (named < count) {
        x -= stackWidth(scale) + kStripGap;
        strips.push_back(
            {.kind = StripKind::Stack, .level = named + 1, .lastLevel = count, .width = stackWidth(scale), .x = x}
        );
        x += stackWidth(scale) + kStripGap;
      }
      for (std::size_t level = named; level >= 1; --level) { // at most kMaxStrips passes
        const double width = bandWidth(level, scale);
        strips.push_back({.kind = StripKind::Ancestor, .level = level, .lastLevel = level, .width = width, .x = x});
        x += width + kStripGap;
      }
      return strips;
    }

    // The drawn width of a saved one, in whole pixels; the saved width itself at 1.
    double drawnWidth(double saved, double scale) { return roundWidth(saved * scale); }

    // The saved width, from 70 to 321, whose drawn width is nearest `drawn` and within `least` to `most` where it can
    // be: a drag or a key step ends at a width that survives saving and drawing again.
    double snappedSaved(double drawn, double scale, double least, double most, double ceiling) {
      const auto at = [&](double saved) { return clampLow(drawnWidth(saved, scale), least, ceiling); };
      double saved = clampLow(roundWidth(drawn / scale), kStripsMinWidth, kStripsMaxWidth);
      for (int i = 0; i < 4 && saved > kStripsMinWidth && at(saved) > most; ++i) { // scale is at most 3
        saved -= 1;
      }
      for (int i = 0; i < 4 && saved < kStripsMaxWidth && at(saved) < least; ++i) {
        saved += 1;
      }
      return saved;
    }

    // The widest pane a center `view` px wide gives the strips while the list keeps `listMin`, never under the least.
    double maxStripsWidth(double view, double listMin, double scale) {
      const double room = std::isfinite(view) ? std::floor(view - listMin) : 0;
      return clampLow(room, stripsMin(scale), stripsMax(scale));
    }

    // The widths after a change that leaves the list `free` px: the narrowest list below 470 is remembered, and
    // forgotten once the list is that wide again.
    ColumnWidths withFloor(const ColumnWidths& before, ColumnWidths after, double free, double listMin) {
      const double floor = std::max(listMin, std::min(free, before.list.value_or(kListPreferred)));
      if (free >= kListPreferred || floor >= kListPreferred) {
        after.list.reset();
      } else {
        after.list = std::max(kListFloor, floor);
      }
      return after;
    }
  } // namespace

  ColumnWidths normalizeWidths(const ColumnWidths& widths) {
    ColumnWidths out;
    for (std::size_t i = 0; i < kColumnsAhead; ++i) {
      const double width = widths.columns[i];
      out.columns[i] = validColumnWidth(width) ? roundWidth(width) : kDefaultColumnWidths[i];
    }
    if (widths.list && std::isfinite(*widths.list) && *widths.list >= kListFloor && *widths.list < kListPreferred) {
      out.list = roundWidth(*widths.list);
    }
    return out;
  }

  std::vector<Strip> ancestorStrips(std::size_t count, double paneWidth, double textScale) {
    const double scale = validScale(textScale);
    const double least = stripsMin(scale);
    const double pane = std::isnan(paneWidth) ? least : clampLow(paneWidth, least, stripsMax(scale));
    if (count <= kMaxStrips && bandsWidth(count, scale) <= pane) {
      return buildStrips(count, count, pane, scale);
    }
    std::size_t named = 1; // the narrowest pane holds one beside the stack
    while (named + 1 < std::min(count, kMaxStrips)
           && bandsWidth(named + 1, scale) + stackWidth(scale) + kStripGap <= pane) { // at most kMaxStrips passes
      ++named;
    }
    return buildStrips(named, count, pane, scale);
  }

  double stripsPaneWidth(std::optional<double> width, double viewWidth, double listMin, double textScale) {
    const double scale = validScale(textScale);
    const double wanted = width && std::isfinite(*width) ? drawnWidth(*width, scale) : stripsDefault(scale);
    return clampLow(wanted, stripsMin(scale), maxStripsWidth(viewWidth, validListMin(listMin), scale));
  }

  std::optional<double> normalizeStripsWidth(std::optional<double> width) {
    if (!width || !std::isfinite(*width)) {
      return std::nullopt;
    }
    const double rounded = roundWidth(*width);
    if (rounded < kStripsMinWidth || rounded > kStripsMaxWidth) {
      return std::nullopt;
    }
    return rounded;
  }

  std::optional<double> resizeStrips(double width, double max, double least, double ceiling) {
    if (std::isnan(width)) {
      return std::nullopt;
    }
    const double hi = std::isnan(max) ? least : clampLow(max, least, ceiling);
    return roundWidth(clampLow(width, least, hi));
  }

  double stripsMinWidthAt(double textScale) { return stripsMin(validScale(textScale)); }

  StripsDrag startStripsDrag(const ColumnPlan& plan, std::optional<double> saved) {
    const double scale = validScale(plan.textScale);
    const double least = plan.stripsMin > 0 ? plan.stripsMin : kStripsMinWidth;
    const double ceiling = plan.stripsCeiling > 0 ? plan.stripsCeiling : kStripsMaxWidth;
    const double fallback = plan.stripsDefault > 0 ? plan.stripsDefault : kStripsDefaultWidth;
    return {
        .startWidth = plan.stripsWidth,
        .startX = plan.margin + plan.stripsWidth,
        .max = plan.stripsMax,
        .saved = saved,
        .wanted =
            clampLow(saved && std::isfinite(*saved) ? drawnWidth(roundWidth(*saved), scale) : fallback, least, ceiling),
        .min = least,
        .ceiling = ceiling,
        .scale = scale,
    };
  }

  double savedStripsWidth(double drawn, double textScale) {
    const double scale = validScale(textScale);
    if (drawn <= stripsMin(scale)) {
      return kStripsMinWidth; // the least pane, which the band widths can make wider than 70 times the scale
    }
    // the least saved width that draws `drawn` or more, so a pane held at its greatest is saved as the least that does
    return clampLow(std::ceil((drawn - 0.5) / scale - 1e-9), kStripsMinWidth, kStripsMaxWidth);
  }

  std::optional<double> dragStrips(const StripsDrag& drag, double pointerX) {
    const auto wanted = resizeStrips(drag.startWidth + (pointerX - drag.startX), drag.max, drag.min, drag.ceiling);
    if (!wanted) {
      return std::nullopt;
    }
    const double most = std::isnan(drag.max) ? drag.min : clampLow(drag.max, drag.min, drag.ceiling);
    const double saved = snappedSaved(*wanted, drag.scale, drag.min, most, drag.ceiling);
    return clampLow(drawnWidth(saved, drag.scale), drag.min, drag.ceiling);
  }

  std::optional<double> stepStrips(const StripsDrag& drag, double edge) {
    if (std::isnan(edge)) {
      return std::nullopt;
    }
    // a step moves the saved width by at least one, whatever the scale
    const double travel = edge - drag.startX;
    const double move =
        travel == 0 ? 0.0 : std::copysign(std::max(1.0, roundWidth(std::abs(travel) / drag.scale)), travel);
    const double from = savedStripsWidth(drag.wanted, drag.scale);
    const double saved = clampLow(from + move, kStripsMinWidth, kStripsMaxWidth);
    return clampLow(drawnWidth(saved, drag.scale), drag.min, drag.ceiling);
  }

  std::size_t columnCount(double room, const std::array<double, kColumnsAhead>& widths, double listMin, double floor) {
    std::size_t count = 0;
    double used = 0;
    for (std::size_t k = 0; k < kColumnsAhead; ++k) {
      used += widths[k] + kStripGap;
      const double need = k == 0 ? listMin : floor;
      if (!(room - used >= need)) {
        break;
      }
      ++count;
    }
    return count;
  }

  double listFloor(const ColumnWidths& widths, double listMin) {
    return std::max(validListMin(listMin), std::min(kListPreferred, widths.list.value_or(kListPreferred)));
  }

  ColumnPlan planColumns(double viewWidth, std::size_t ancestors, const PlanOptions& options) {
    const double view = std::isfinite(viewWidth) && viewWidth > 0 ? viewWidth : 0;
    const double listMin = validListMin(options.listMin);
    const auto widths = normalizeWidths(options.widths);
    const double scale = validScale(options.textScale);
    ColumnPlan plan;
    if (options.strips) {
      plan.stripsWidth = stripsPaneWidth(options.stripsWidth, view, listMin, scale);
      plan.stripsMax = maxStripsWidth(view, listMin, scale);
      plan.stripsDefault = stripsDefault(scale);
      plan.stripsMin = stripsMin(scale);
      plan.stripsCeiling = stripsMax(scale);
      plan.textScale = scale;
      plan.strips = ancestorStrips(ancestors, plan.stripsWidth, scale);
    }
    const auto fit = columnCount(view - plan.stripsWidth, widths.columns, listMin, listFloor(widths, listMin));
    const auto count = std::min(fit, options.maxColumns.value_or(kColumnsAhead));
    double columnsWidth = 0;
    for (std::size_t i = 0; i < count; ++i) {
      columnsWidth += widths.columns[i] + kStripGap;
    }
    const double free = view - plan.stripsWidth - columnsWidth;
    plan.list.width = std::max(0.0, std::min(kListMax, free));
    plan.margin = std::max(0.0, std::floor((free - plan.list.width) / 2));
    plan.spare = free - listMin;
    for (auto& strip : plan.strips) {
      strip.x += plan.margin;
    }
    double x = plan.margin + plan.stripsWidth;
    plan.list.x = x;
    x += plan.list.width;
    for (std::size_t i = 0; i < count; ++i) {
      plan.columns.push_back({.index = i, .x = x, .width = widths.columns[i]});
      x += widths.columns[i] + kStripGap;
    }
    return plan;
  }

  std::optional<double> maxColumnWidth(const ColumnPlan& plan, std::size_t column) {
    if (column >= plan.columns.size()) {
      return std::nullopt;
    }
    return clampLow(plan.columns[column].width + plan.spare, kColumnMin, kColumnMax);
  }

  ColumnWidths
  resizeColumn(const ColumnWidths& widths, const ColumnPlan& plan, std::size_t column, double width, double listMin) {
    const auto max = maxColumnWidth(plan, column);
    if (!max || column >= kColumnsAhead || std::isnan(width)) {
      return widths;
    }
    const double value = roundWidth(clampLow(width, kColumnMin, *max));
    if (value == widths.columns[column]) {
      return widths;
    }
    auto next = widths;
    next.columns[column] = value;
    const double min = validListMin(listMin);
    return withFloor(widths, next, plan.spare + min - (value - plan.columns[column].width), min);
  }

  std::optional<ColumnDrag>
  startColumnDrag(const ColumnPlan& plan, std::size_t column, double pointerX, const ColumnWidths& widths) {
    if (column >= plan.columns.size()) {
      return std::nullopt;
    }
    return ColumnDrag{
        .column = column,
        .startWidth = plan.columns[column].width,
        .startX = pointerX,
        .widths = widths,
        .spare = plan.spare,
    };
  }

  ColumnWidths dragColumn(const ColumnDrag& drag, double pointerX, double listMin) {
    const double target = drag.startWidth + (drag.startX - pointerX);
    if (drag.column >= kColumnsAhead || std::isnan(target)) {
      return drag.widths;
    }
    const double max = clampLow(drag.startWidth + drag.spare, kColumnMin, kColumnMax);
    const double value = roundWidth(clampLow(target, kColumnMin, max));
    if (value == drag.widths.columns[drag.column]) {
      return drag.widths;
    }
    auto next = drag.widths;
    next.columns[drag.column] = value;
    const double min = validListMin(listMin);
    return withFloor(drag.widths, next, drag.spare + min - (value - drag.startWidth), min);
  }

  double columnFont(double listFont, std::size_t column) {
    constexpr double kSmallest = 11;
    if (!std::isfinite(listFont)) {
      return kSmallest;
    }
    return std::max(kSmallest, listFont - 1 - 0.5 * static_cast<double>(column));
  }

  DetailsColumns
  fitListColumns(double viewport, const DetailsColumnVisibility& chosen, const DetailsColumnWidths& widths) {
    constexpr std::array kLeftOut{
        DetailsColumn::Location, DetailsColumn::Permissions, DetailsColumn::Group,
        DetailsColumn::Owner,    DetailsColumn::Created,     DetailsColumn::Accessed,
        DetailsColumn::Type,     DetailsColumn::Modified,    DetailsColumn::Size,
    };
    auto requested = widths;
    requested[0] = 0; // Name takes the room that is left
    for (std::size_t i = 1; i < requested.size(); ++i) {
      requested[i] = std::clamp(requested[i], minimumDetailsColumnWidth(i), maximumDetailsColumnWidth(i));
    }
    const auto room = static_cast<float>(std::isfinite(viewport) && viewport > 0 ? viewport : 0);
    auto shown = chosen;
    auto columns = DetailsColumns::forWidth(room, shown, requested);
    for (const auto column : kLeftOut) { // at most one pass per column
      if (columns.contentWidth <= room) {
        break;
      }
      if (detailsColumnVisible(shown, column)) {
        detailsColumnFlag(shown, column) = false;
        columns = DetailsColumns::forWidth(room, shown, requested);
      }
    }
    return columns;
  }

  ListHeader
  listHeader(ViewMode mode, double viewport, const DetailsColumnVisibility& chosen, const DetailsColumnWidths& widths) {
    if (mode == ViewMode::Workbench) {
      return {.columns = fitListColumns(viewport, chosen, widths), .resizable = false};
    }
    const auto room = static_cast<float>(std::isfinite(viewport) ? viewport : 0);
    return {.columns = DetailsColumns::forWidth(room, chosen, widths), .resizable = mode == ViewMode::List};
  }

  bool columnLeftOut(
      ViewMode mode, const DetailsColumns& shown, const DetailsColumnVisibility& chosen, DetailsColumn column
  ) {
    return mode == ViewMode::Workbench && detailsColumnVisible(chosen, column) && !shown.visible(column);
  }

  ColumnChain columnChain(
      const Tree& tree, const LayoutSettings& settings, const ChainFocus& focus, std::span<const NodeId> remembered
  ) {
    const auto isFolder = [&tree](NodeId id) { return tree.nodes[id].folder; };
    const auto heaviest = [&](NodeId folder) {
      for (const auto id : entries(tree, folder, settings).ids) {
        if (isFolder(id)) {
          return id;
        }
      }
      return kNoNode;
    };
    const auto rememberedIn = [&](NodeId folder) {
      for (const auto id : remembered) {
        if (hasNode(tree, id) && tree.nodes[id].parent == folder) {
          const bool shown = isFolder(id) && visibleEntry(tree.nodes[id], settings.showHidden);
          return shown ? id : kNoNode;
        }
      }
      return kNoNode;
    };
    NodeId pick = kNoNode;
    if (hasNode(tree, focus.pointer)) {
      pick = focus.pointer;
    } else if (hasNode(tree, focus.cursor)) {
      pick = focus.cursor;
    }
    const auto next = [&](NodeId folder) {
      if (!hasNode(tree, folder)) {
        return kNoNode;
      }
      const auto trail = pick == kNoNode ? rememberedIn(folder) : kNoNode;
      return trail != kNoNode ? trail : heaviest(folder);
    };
    const auto steered = [&tree](NodeId steer, NodeId parent) {
      return hasNode(tree, steer) && tree.nodes[steer].parent == parent;
    };
    ColumnChain chain;
    if (pick != kNoNode && !isFolder(pick)) {
      chain.file = pick;
      return chain;
    }
    chain.a = pick != kNoNode ? pick : next(tree.root);
    if (chain.a == kNoNode) {
      return chain;
    }
    chain.b = steered(focus.steerB, chain.a) ? focus.steerB : next(chain.a);
    if (chain.b == kNoNode || !isFolder(chain.b)) {
      return chain;
    }
    chain.c = steered(focus.steerC, chain.b) ? focus.steerC : next(chain.b);
    return chain;
  }
} // namespace luciole::workbench
