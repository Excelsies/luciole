#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace luciole::workbench {
  inline constexpr double kPaneHeader = 24;
  inline constexpr double kRailWidth = 32;
  inline constexpr double kLeftMin = 200;
  inline constexpr double kCenterMinWidth = 390; // the list's 320 px beside the narrowest ancestor strips pane, 70 px
  inline constexpr double kCenterMinHeight = 160;
  inline constexpr double kJumpMinBody = 52;
  inline constexpr double kSavedSizeMax = 100000;

  enum class RibbonKind : std::uint8_t { Flame, Flow, Ladder };
  inline constexpr std::size_t kRibbonKindCount = 3;

  enum class Pane : std::uint8_t { Instrument, Jump, Ribbon };
  enum class DividerId : std::uint8_t { Left, Instrument, Ribbon };
  enum class DividerAxis : std::uint8_t { Vertical, Horizontal };
  enum class ForcedReason : std::uint8_t { None, Short, Narrow, ShortAndNarrow };

  struct PaneFlags {
    bool instrument = false;
    bool jump = false;
    bool ribbon = false;
    bool operator==(const PaneFlags&) const = default;
  };

  // Saved pane sizes in pixels, headers included. An empty size follows the view.
  struct PaneState {
    std::optional<double> leftWidth;
    std::optional<double> instrumentHeight;
    PaneFlags collapsed;
    bool expanded = false;
    std::array<std::optional<double>, kRibbonKindCount> ribbonHeights; // indexed by RibbonKind
    std::optional<std::array<double, 3>> columnWidths;                 // columns ahead, each 140 to 640
    bool operator==(const PaneState&) const = default;
  };

  struct SanitizedPaneState {
    PaneState state;
    std::vector<std::string> problems; // one sentence per field that was replaced by its default
  };

  // Body sizes a part needs, below its header.
  struct InstrumentLimits {
    double minWidth = 240;
    double minHeight = 220;
    bool operator==(const InstrumentLimits&) const = default;
  };

  struct RibbonLimits {
    double minWidth = 0;
    double minHeight = 120;
    double preferredHeight = 200;
    double maxHeight = std::numeric_limits<double>::infinity();
    bool operator==(const RibbonLimits&) const = default;
  };

  // What the panes need: the parts' body sizes, the height of a pane's header and the Jump ahead pane's least body.
  // The last two grow with the text, so the owner of the panes sets them.
  struct PartLimits {
    InstrumentLimits instrument;
    RibbonLimits ribbon;
    double header = kPaneHeader;
    double jumpMinBody = kJumpMinBody;
    bool operator==(const PartLimits&) const = default;
  };

  // The limits with every size that holds text multiplied by `scale`, the size of the system font over its default;
  // the maximum height stays as it is. A scale that is not finite and positive changes nothing.
  PartLimits scaleLimits(const PartLimits& limits, double scale);

  // Limits for a pane whose part is not mounted yet.
  inline constexpr InstrumentLimits kPlanningInstrumentLimits{.minWidth = 240, .minHeight = 220};
  RibbonLimits planningRibbonLimits(RibbonKind kind);

  struct ViewSize {
    double width = 0;
    double height = 0;
  };

  struct PaneRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool operator==(const PaneRect&) const = default;
  };

  struct PaneRects {
    std::optional<PaneRect> instrument;
    std::optional<PaneRect> jump;
    std::optional<PaneRect> ribbon;
    std::optional<PaneRect> center;
    std::optional<PaneRect> rail;
    bool operator==(const PaneRects&) const = default;
  };

  struct PaneReasons {
    ForcedReason instrument = ForcedReason::None;
    ForcedReason jump = ForcedReason::None;
    ForcedReason ribbon = ForcedReason::None;
  };

  // A divider sits at (x, y) and runs `length` along its axis; a draggable one moves its value within min..max.
  struct Divider {
    DividerId id = DividerId::Left;
    DividerAxis axis = DividerAxis::Vertical;
    double x = 0;
    double y = 0;
    double length = 0;
    bool draggable = false;
    double min = 0;
    double max = 0;
    double value = 0;
  };

  struct PaneLayout {
    PaneState state; // the sanitized input; layout never rewrites saved sizes
    ViewSize view;   // whole, non-negative pixels
    bool expanded = false;
    PaneRects rects;                // the visible rects tile the view exactly
    std::optional<PaneRects> under; // while expanded: where the hidden panes stay
    PaneFlags collapsed;            // shown collapsed, by choice or by force
    PaneFlags forced;               // closed because the view is too small, not by choice
    PaneReasons why;
    bool rail = false;
    std::vector<Divider> dividers; // left, then instrument (absent on the rail), then ribbon
    bool squeezed = false;         // an open pane is still under its minimum
  };

  struct DividerDrag {
    PaneState state;
    double value = 0;
  };

  // Each size outside its range becomes its default and adds a problem; an expanded state with a collapsed
  // instrument is no longer expanded.
  SanitizedPaneState sanitizePaneState(const PaneState& state);

  // Non-finite or negative limits read as their defaults, and the ribbon's maximum is never under its minimum.
  PartLimits normalizeLimits(const PartLimits& limits);

  // About 22% of the view width, between 340 and 520.
  double defaultLeftWidth(double viewWidth);

  // Pane rectangles for a view. Too small a view closes the ribbon, then Jump ahead, then turns the left column
  // into the rail.
  PaneLayout layoutPanes(const PaneState& state, ViewSize view, const PartLimits& limits, RibbonKind kind);

  // The part of a pane below its header; empty for a hidden or collapsed pane.
  std::optional<PaneRect> paneBody(const std::optional<PaneRect>& pane, bool collapsed);

  // The divider's size set to a value the caller already clamped; the ribbon height is kept per kind.
  PaneState withDividerValue(const PaneState& state, DividerId id, double value, RibbonKind kind);

  // The divider's size back to following the view.
  PaneState withDividerDefault(const PaneState& state, DividerId id, RibbonKind kind);

  // One frame of a drag: the value is the start value plus the pointer travel, clamped by the divider. Both start
  // values are captured at pointer down. A divider that cannot be dragged, or a NaN travel, changes nothing.
  DividerDrag dragDivider(
      const PaneState& startState, ViewSize view, const PartLimits& limits, RibbonKind kind, DividerId id,
      double startValue, double delta
  );

  // Collapses or restores one pane; collapsing the instrument also ends its expansion.
  PaneState togglePane(const PaneState& state, Pane pane);

  // Expanding the instrument also restores it.
  PaneState withExpanded(const PaneState& state, bool expanded);
} // namespace luciole::workbench
