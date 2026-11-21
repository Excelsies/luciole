#include "workbench/pane_layout.h"

#include "workbench/column_plan.h"
#include "workbench/rounding.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace luciole::workbench {
  namespace {
    constexpr double kLeftAutoMin = 340;
    constexpr double kLeftAutoMax = 520;
    constexpr double kLeftAutoShare = 0.22;
    constexpr double kInstrumentShare = 0.62;
    constexpr double kRibbonShare = 0.38;
    constexpr std::array<std::string_view, kRibbonKindCount> kRibbonNames{"flame", "flow", "ladder"};

    struct DividerRange {
      double min = 0;
      double max = 0;
      double value = 0;
    };

    // Heights of the left column's panes, headers included.
    struct LeftSplit {
      double instrumentHeight = 0;
      double jumpHeight = 0;
      bool forcedJump = false;
      std::optional<DividerRange> divider;
    };

    struct WidthSplit {
      bool rail = false;
      bool railForced = false;
      double leftWidth = 0;
      bool forcedRibbon = false;
      std::optional<DividerRange> divider;
    };

    // Value within lo..hi; when hi < lo the minimum wins.
    double clampLow(double value, double lo, double hi) { return std::min(std::max(value, lo), std::max(lo, hi)); }

    PaneRect makeRect(double x, double y, double width, double height) {
      return {
          .x = roundHalfUp(x),
          .y = roundHalfUp(y),
          .width = std::max(0.0, roundHalfUp(width)),
          .height = std::max(0.0, roundHalfUp(height)),
      };
    }

    // A rect cut to the view: on a view smaller than a header or the rail a pane shrinks to nothing.
    PaneRect inside(const PaneRect& rect, double width, double height) {
      const double x = std::min(rect.x, width);
      const double y = std::min(rect.y, height);
      return {.x = x, .y = y, .width = std::min(rect.width, width - x), .height = std::min(rect.height, height - y)};
    }

    Divider makeDivider(
        DividerId id, DividerAxis axis, double x, double y, double length, const std::optional<DividerRange>& range
    ) {
      Divider divider{
          .id = id,
          .axis = axis,
          .x = roundHalfUp(x),
          .y = roundHalfUp(y),
          .length = std::max(0.0, roundHalfUp(length)),
      };
      if (range) {
        divider.draggable = true;
        divider.min = range->min;
        divider.max = range->max;
        divider.value = range->value;
      }
      return divider;
    }

    std::size_t ribbonIndex(RibbonKind kind) {
      const auto index = static_cast<std::size_t>(kind);
      return index < kRibbonKindCount ? index : 0;
    }

    double wholeSize(double value) { return std::isfinite(value) ? std::max(0.0, std::floor(value)) : 0; }

    std::optional<double>
    sizeField(const std::optional<double>& value, std::string_view name, std::vector<std::string>& problems) {
      if (!value || (std::isfinite(*value) && *value >= 0 && *value <= kSavedSizeMax)) {
        return value;
      }
      problems.push_back(std::format("{} must be a number from 0 to {}, got {}", name, kSavedSizeMax, *value));
      return std::nullopt;
    }

    std::optional<std::array<double, 3>>
    columnsField(const std::optional<std::array<double, 3>>& value, std::vector<std::string>& problems) {
      if (!value) {
        return std::nullopt;
      }
      const auto& widths = *value;
      if (std::ranges::all_of(widths, [](double width) {
            return std::isfinite(width) && width >= kColumnMin && width <= kColumnMax;
          })) {
        return std::array{roundHalfUp(widths[0]), roundHalfUp(widths[1]), roundHalfUp(widths[2])};
      }
      problems.push_back(
          std::format(
              "columnWidths must each be from {} to {}, got {}, {} and {}", kColumnMin, kColumnMax, widths[0],
              widths[1], widths[2]
          )
      );
      return std::nullopt;
    }

    LeftSplit splitLeft(const PaneState& state, double height, const PartLimits& limits) {
      const bool instrumentOpen = !state.collapsed.instrument;
      const bool jumpOpen = !state.collapsed.jump;
      const double instrumentMin = limits.header + limits.instrument.minHeight;
      const double jumpMin = limits.header + limits.jumpMinBody;
      const double header = std::min(limits.header, height);
      const double rest = std::max(0.0, height - limits.header);
      if (instrumentOpen && jumpOpen) {
        if (height < instrumentMin + jumpMin) {
          return {.instrumentHeight = rest, .jumpHeight = header, .forcedJump = true, .divider = std::nullopt};
        }
        const double hi = height - jumpMin;
        const double saved = state.instrumentHeight.value_or(roundHalfUp(height * kInstrumentShare));
        const double value = clampLow(saved, instrumentMin, hi);
        return {
            .instrumentHeight = value,
            .jumpHeight = height - value,
            .forcedJump = false,
            .divider = DividerRange{.min = instrumentMin, .max = hi, .value = value},
        };
      }
      if (instrumentOpen) {
        return {.instrumentHeight = rest, .jumpHeight = header, .forcedJump = false, .divider = std::nullopt};
      }
      return {.instrumentHeight = header, .jumpHeight = rest, .forcedJump = false, .divider = std::nullopt};
    }

    WidthSplit splitWidth(const PaneState& state, double width, const PartLimits& limits, bool ribbonOpen) {
      const bool instrumentOpen = !state.collapsed.instrument;
      if (!instrumentOpen && state.collapsed.jump) {
        return {.rail = true, .railForced = false, .leftWidth = kRailWidth, .forcedRibbon = false, .divider = {}};
      }
      const double leftMin = std::max(kLeftMin, instrumentOpen ? limits.instrument.minWidth : 0);
      bool forcedRibbon = false;
      double rightMin = std::max(limits.centerMinWidth, ribbonOpen ? limits.ribbon.minWidth : 0);
      // a ribbon wider than the center is closed before the left column becomes the rail
      if (width - rightMin < leftMin && ribbonOpen && limits.ribbon.minWidth > limits.centerMinWidth) {
        forcedRibbon = true;
        rightMin = limits.centerMinWidth;
      }
      const double leftMax = width - rightMin;
      if (leftMax < leftMin) {
        return {.rail = true, .railForced = true, .leftWidth = kRailWidth, .forcedRibbon = forcedRibbon, .divider = {}};
      }
      const double value = clampLow(state.leftWidth.value_or(defaultLeftWidth(width)), leftMin, leftMax);
      return {
          .rail = false,
          .railForced = false,
          .leftWidth = value,
          .forcedRibbon = forcedRibbon,
          .divider = DividerRange{.min = leftMin, .max = leftMax, .value = value},
      };
    }

    void placeLeft(PaneLayout& out, const PaneState& state, const WidthSplit& widths, const LeftSplit& heights) {
      const double height = out.view.height;
      if (widths.rail) {
        out.rects.rail = makeRect(0, 0, kRailWidth, height);
        out.rail = true;
        if (widths.railForced) {
          out.forced.instrument = !state.collapsed.instrument;
          out.forced.jump = !state.collapsed.jump;
        }
        out.collapsed.instrument = true;
        out.collapsed.jump = true;
        out.dividers.push_back(makeDivider(DividerId::Left, DividerAxis::Vertical, kRailWidth, 0, height, {}));
        return;
      }
      out.forced.jump = heights.forcedJump;
      out.collapsed.jump = out.collapsed.jump || heights.forcedJump;
      out.rects.instrument = makeRect(0, 0, widths.leftWidth, heights.instrumentHeight);
      out.rects.jump = makeRect(0, heights.instrumentHeight, widths.leftWidth, heights.jumpHeight);
      out.dividers.push_back(
          makeDivider(DividerId::Left, DividerAxis::Vertical, widths.leftWidth, 0, height, widths.divider)
      );
      out.dividers.push_back(makeDivider(
          DividerId::Instrument, DividerAxis::Horizontal, 0, heights.instrumentHeight, widths.leftWidth, heights.divider
      ));
    }

    void placeRight(
        PaneLayout& out, const PaneState& state, double left, bool ribbonOpen, const PartLimits& limits,
        std::size_t kind
    ) {
      const double width = out.view.width - left;
      const double height = out.view.height;
      if (!ribbonOpen) {
        const double header = std::min(limits.header, height);
        out.rects.ribbon = makeRect(left, 0, width, header);
        out.rects.center = makeRect(left, limits.header, width, height - limits.header);
        out.dividers.push_back(makeDivider(DividerId::Ribbon, DividerAxis::Horizontal, left, header, width, {}));
        return;
      }
      const double lo = limits.header + limits.ribbon.minHeight;
      const double hi = std::min(height - kCenterMinHeight, limits.header + limits.ribbon.maxHeight);
      // by default the part's preferred height, but at most 38% of the view
      const double fallback =
          std::min(limits.header + limits.ribbon.preferredHeight, roundHalfUp(height * kRibbonShare));
      const double value = clampLow(state.ribbonHeights[kind].value_or(fallback), lo, hi);
      out.rects.ribbon = makeRect(left, 0, width, value);
      out.rects.center = makeRect(left, value, width, height - value);
      out.dividers.push_back(makeDivider(
          DividerId::Ribbon, DividerAxis::Horizontal, left, value, width,
          DividerRange{.min = lo, .max = hi, .value = value}
      ));
    }

    // True when an open pane is smaller than its part's minimum, which only a very small view causes.
    bool squeezedIn(const PaneLayout& out, const PartLimits& limits) {
      const auto small = [](const std::optional<PaneRect>& box, double minWidth, double minHeight) {
        return box && (box->width < minWidth || box->height < minHeight);
      };
      const auto& rects = out.rects;
      if (!out.collapsed.instrument
          && small(rects.instrument, limits.instrument.minWidth, limits.header + limits.instrument.minHeight)) {
        return true;
      }
      if (!out.collapsed.jump && small(rects.jump, 0, limits.header + limits.jumpMinBody)) {
        return true;
      }
      if (!out.collapsed.ribbon
          && small(rects.ribbon, limits.ribbon.minWidth, limits.header + limits.ribbon.minHeight)) {
        return true;
      }
      return small(rects.center, limits.centerMinWidth, kCenterMinHeight);
    }

    ForcedReason ribbonReason(bool room, bool narrow) {
      if (!room && narrow) {
        return ForcedReason::ShortAndNarrow;
      }
      return room ? ForcedReason::Narrow : ForcedReason::Short;
    }

    // The panes of a state that is not expanded.
    PaneLayout arrange(const PaneState& state, ViewSize view, const PartLimits& limits, RibbonKind kind) {
      PaneLayout out;
      out.state = state;
      out.view = view;
      out.collapsed = state.collapsed;
      const double width = view.width;
      const double height = view.height;
      const bool ribbonRoom = height >= limits.header + limits.ribbon.minHeight + kCenterMinHeight;
      bool ribbonOpen = !state.collapsed.ribbon && ribbonRoom;
      const auto widths = splitWidth(state, width, limits, ribbonOpen);
      ribbonOpen = ribbonOpen && !widths.forcedRibbon;
      const auto heights = splitLeft(state, height, limits);
      out.forced.ribbon = !state.collapsed.ribbon && !ribbonOpen;
      if (out.forced.ribbon) {
        // name every side that is too small, so growing the window one way never leaves a new reason behind
        const bool narrow = widths.forcedRibbon || splitWidth(state, width, limits, true).forcedRibbon;
        out.why.ribbon = ribbonReason(ribbonRoom, narrow);
      }
      if (widths.railForced) {
        out.why.instrument = state.collapsed.instrument ? ForcedReason::None : ForcedReason::Narrow;
        out.why.jump = state.collapsed.jump ? ForcedReason::None : ForcedReason::Narrow;
      }
      if (heights.forcedJump && !widths.rail) {
        out.why.jump = ForcedReason::Short;
      }
      out.collapsed.ribbon = !ribbonOpen;
      placeLeft(out, state, widths, heights);
      placeRight(out, state, widths.leftWidth, ribbonOpen, limits, ribbonIndex(kind));
      out.squeezed = squeezedIn(out, limits);
      for (auto* rect :
           {&out.rects.instrument, &out.rects.jump, &out.rects.ribbon, &out.rects.center, &out.rects.rail}) {
        if (*rect) {
          *rect = inside(**rect, width, height);
        }
      }
      return out;
    }
  } // namespace

  RibbonLimits planningRibbonLimits(RibbonKind kind) {
    switch (kind) {
    case RibbonKind::Flow:
      return {.minWidth = 560, .minHeight = 180, .preferredHeight = 280, .maxHeight = 420};
    case RibbonKind::Ladder:
      return {.minWidth = 360, .minHeight = 76, .preferredHeight = 160, .maxHeight = 260};
    case RibbonKind::Flame:
    default:
      return {.minWidth = 480, .minHeight = 160, .preferredHeight = 224, .maxHeight = 281};
    }
  }

  SanitizedPaneState sanitizePaneState(const PaneState& state) {
    SanitizedPaneState out;
    auto& clean = out.state;
    clean.leftWidth = sizeField(state.leftWidth, "leftWidth", out.problems);
    clean.instrumentHeight = sizeField(state.instrumentHeight, "instrumentHeight", out.problems);
    clean.collapsed = state.collapsed;
    clean.expanded = state.expanded && !state.collapsed.instrument; // an expanded pane is open
    for (std::size_t kind = 0; kind < kRibbonKindCount; ++kind) {
      const auto name = std::format("ribbonHeights.{}", kRibbonNames[kind]);
      clean.ribbonHeights[kind] = sizeField(state.ribbonHeights[kind], name, out.problems);
    }
    clean.columnWidths = columnsField(state.columnWidths, out.problems);
    return out;
  }

  PartLimits scaleLimits(const PartLimits& limits, double scale) {
    if (!std::isfinite(scale) || !(scale > 0) || scale == 1.0) {
      return limits; // at the default size the limits are the part's own, whole or not
    }
    const auto whole = [scale](double value) { return std::round(value * scale); };
    PartLimits out = limits;
    out.instrument.minWidth = whole(out.instrument.minWidth);
    out.instrument.minHeight = whole(out.instrument.minHeight);
    out.ribbon.minWidth = whole(out.ribbon.minWidth);
    out.ribbon.minHeight = whole(out.ribbon.minHeight);
    out.ribbon.preferredHeight = whole(out.ribbon.preferredHeight);
    out.ribbon.maxHeight = whole(out.ribbon.maxHeight);
    out.header = whole(out.header);
    out.jumpMinBody = whole(out.jumpMinBody);
    out.centerMinWidth = kListFloor + stripsMinWidthAt(scale);
    return out;
  }

  PartLimits normalizeLimits(const PartLimits& limits) {
    const auto valid = [](double value, double fallback) {
      return std::isfinite(value) && value >= 0 ? value : fallback;
    };
    const PartLimits defaults;
    const auto& ribbon = limits.ribbon;
    const double minHeight = valid(ribbon.minHeight, defaults.ribbon.minHeight);
    const double maxHeight = std::isfinite(ribbon.maxHeight) && ribbon.maxHeight > 0
        ? std::max(minHeight, ribbon.maxHeight)
        : defaults.ribbon.maxHeight;
    return {
        .instrument =
            {
                .minWidth = valid(limits.instrument.minWidth, defaults.instrument.minWidth),
                .minHeight = valid(limits.instrument.minHeight, defaults.instrument.minHeight),
            },
        .ribbon =
            {
                .minWidth = valid(ribbon.minWidth, defaults.ribbon.minWidth),
                .minHeight = minHeight,
                .preferredHeight =
                    clampLow(valid(ribbon.preferredHeight, defaults.ribbon.preferredHeight), minHeight, maxHeight),
                .maxHeight = maxHeight,
            },
        .header = valid(limits.header, defaults.header),
        .jumpMinBody = valid(limits.jumpMinBody, defaults.jumpMinBody),
        .centerMinWidth = valid(limits.centerMinWidth, defaults.centerMinWidth),
    };
  }

  double defaultLeftWidth(double viewWidth) {
    if (std::isnan(viewWidth)) {
      return kLeftAutoMin;
    }
    return clampLow(roundHalfUp(viewWidth * kLeftAutoShare), kLeftAutoMin, kLeftAutoMax);
  }

  PaneLayout layoutPanes(const PaneState& state, ViewSize view, const PartLimits& limits, RibbonKind kind) {
    const auto clean = sanitizePaneState(state).state;
    const auto parts = normalizeLimits(limits);
    const ViewSize size{.width = wholeSize(view.width), .height = wholeSize(view.height)};
    auto folded = clean;
    folded.expanded = false;
    auto out = arrange(folded, size, parts, kind);
    out.state = clean;
    if (!clean.expanded) {
      return out;
    }
    // the instrument covers the view; the other panes keep their place so they can stay mounted, hidden
    auto under = out.rects;
    under.instrument.reset();
    out.under = under;
    out.expanded = true;
    out.collapsed.instrument = false;
    out.forced.instrument = false;
    out.why.instrument = ForcedReason::None;
    out.rects = PaneRects{};
    out.rects.instrument = makeRect(0, 0, size.width, size.height);
    out.dividers.clear();
    out.squeezed = size.width < parts.instrument.minWidth || size.height < parts.header + parts.instrument.minHeight;
    return out;
  }

  PaneState withDividerValue(const PaneState& state, DividerId id, double value, RibbonKind kind) {
    auto clean = sanitizePaneState(state).state;
    if (!std::isfinite(value)) {
      return clean;
    }
    const double rounded = roundHalfUp(value);
    switch (id) {
    case DividerId::Left:
      clean.leftWidth = rounded;
      break;
    case DividerId::Instrument:
      clean.instrumentHeight = rounded;
      break;
    case DividerId::Ribbon:
      clean.ribbonHeights[ribbonIndex(kind)] = rounded;
      break;
    }
    return clean;
  }

  PaneState withDividerDefault(const PaneState& state, DividerId id, RibbonKind kind) {
    auto clean = sanitizePaneState(state).state;
    switch (id) {
    case DividerId::Left:
      clean.leftWidth.reset();
      break;
    case DividerId::Instrument:
      clean.instrumentHeight.reset();
      break;
    case DividerId::Ribbon:
      clean.ribbonHeights[ribbonIndex(kind)].reset();
      break;
    }
    return clean;
  }

  DividerDrag dragDivider(
      const PaneState& startState, ViewSize view, const PartLimits& limits, RibbonKind kind, DividerId id,
      double startValue, double delta
  ) {
    const auto layout = layoutPanes(startState, view, limits, kind);
    const auto found = std::ranges::find(layout.dividers, id, &Divider::id);
    if (found == layout.dividers.end()) {
      return {.state = layout.state, .value = 0};
    }
    const double target = startValue + delta;
    if (!found->draggable || std::isnan(target)) {
      return {.state = layout.state, .value = found->value};
    }
    const double value = clampLow(roundHalfUp(target), found->min, found->max);
    return {.state = withDividerValue(startState, id, value, kind), .value = value};
  }

  PaneState togglePane(const PaneState& state, Pane pane) {
    auto clean = sanitizePaneState(state).state;
    switch (pane) {
    case Pane::Instrument:
      clean.collapsed.instrument = !clean.collapsed.instrument;
      clean.expanded = clean.expanded && !clean.collapsed.instrument;
      break;
    case Pane::Jump:
      clean.collapsed.jump = !clean.collapsed.jump;
      break;
    case Pane::Ribbon:
      clean.collapsed.ribbon = !clean.collapsed.ribbon;
      break;
    }
    return clean;
  }

  PaneState withExpanded(const PaneState& state, bool expanded) {
    auto clean = sanitizePaneState(state).state;
    clean.expanded = expanded;
    if (expanded) {
      clean.collapsed.instrument = false;
    }
    return clean;
  }
} // namespace luciole::workbench
