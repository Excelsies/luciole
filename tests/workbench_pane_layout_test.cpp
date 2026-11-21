#include "test_check.h"
#include "workbench/pane_layout.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  const PartLimits kLim{
      .instrument = {.minWidth = 240, .minHeight = 220},
      .ribbon = {.minWidth = 360, .minHeight = 140, .preferredHeight = 200, .maxHeight = 360},
  };
  const PartLimits kFlow{
      .instrument = {.minWidth = 240, .minHeight = 220},
      .ribbon = {.minWidth = 560, .minHeight = 180, .preferredHeight = 280, .maxHeight = 420},
  };
  const std::array<PartLimits, 5> kHostileLimits{
      PartLimits{},
      PartLimits{},
      PartLimits{
          .instrument = {.minWidth = kNan, .minHeight = -4},
          .ribbon = {.minWidth = kNan, .minHeight = kInfinity, .preferredHeight = -1, .maxHeight = 3},
      },
      PartLimits{
          .instrument = {.minWidth = 5000, .minHeight = 5000},
          .ribbon = {.minWidth = 5000, .minHeight = 5000, .preferredHeight = 9e9, .maxHeight = 1},
      },
      PartLimits{
          .instrument = {},
          .ribbon = {.minWidth = 0, .minHeight = 100, .preferredHeight = 200, .maxHeight = 50},
      },
  };

  std::array<std::optional<PaneRect>, 5> rectList(const PaneRects& rects) {
    return {rects.instrument, rects.jump, rects.ribbon, rects.center, rects.rail};
  }

  PaneRect rectOf(const std::optional<PaneRect>& rect) {
    TEST_CHECK(rect.has_value());
    return *rect;
  }

  const Divider& dividerOf(const PaneLayout& layout, DividerId id) {
    const Divider* found = nullptr;
    for (const auto& divider : layout.dividers) {
      if (divider.id == id) {
        found = &divider;
      }
    }
    TEST_CHECK(found != nullptr);
    return *found;
  }

  bool overlaps(const PaneRect& a, const PaneRect& b) {
    return std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x) > 0
        && std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y) > 0;
  }

  bool wholePixel(double value) { return value >= 0 && std::floor(value) == value; }

  // Whole non-negative pixels, inside the view, no overlap, together exactly the view.
  void checkTiles(const PaneLayout& layout, std::string_view label) {
    const auto rects = rectList(layout.rects);
    double total = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
      if (!rects[i]) {
        continue;
      }
      const auto& rect = *rects[i];
      const bool whole = wholePixel(rect.x) && wholePixel(rect.y) && wholePixel(rect.width) && wholePixel(rect.height);
      const bool inside = rect.x + rect.width <= layout.view.width && rect.y + rect.height <= layout.view.height;
      bool apart = true;
      for (std::size_t j = i + 1; j < rects.size(); ++j) {
        apart = apart && !(rects[j] && overlaps(rect, *rects[j]));
      }
      if (!whole || !inside || !apart) {
        std::println(stderr, "{}: pane {} at {} {} {} {}", label, i, rect.x, rect.y, rect.width, rect.height);
      }
      TEST_CHECK(whole);
      TEST_CHECK(inside);
      TEST_CHECK(apart);
      total += rect.width * rect.height;
    }
    const bool covered = total == layout.view.width * layout.view.height;
    if (!covered) {
      std::println(stderr, "{}: the panes cover {} of {}", label, total, layout.view.width * layout.view.height);
    }
    TEST_CHECK(covered);
  }

  // Every open pane has at least its minimum; called only where the view is big enough for them.
  void checkMinimums(const PaneLayout& layout, const PartLimits& limits, std::string_view label) {
    if (layout.expanded) {
      return;
    }
    const auto& rects = layout.rects;
    bool ok = true;
    if (!layout.collapsed.instrument) {
      const auto box = rectOf(rects.instrument);
      ok = ok && box.width >= limits.instrument.minWidth && box.height >= kPaneHeader + limits.instrument.minHeight;
    }
    if (!layout.collapsed.jump) {
      ok = ok && rectOf(rects.jump).height >= kPaneHeader + kJumpMinBody;
    }
    if (!layout.collapsed.ribbon) {
      const auto box = rectOf(rects.ribbon);
      ok = ok && box.height >= kPaneHeader + limits.ribbon.minHeight && box.width >= limits.ribbon.minWidth;
    }
    const auto center = rectOf(rects.center);
    ok = ok && center.width >= kCenterMinWidth && center.height >= kCenterMinHeight;
    if (!ok) {
      std::println(stderr, "{}: a pane is under its minimum", label);
    }
    TEST_CHECK(ok);
  }

  PaneLayout layoutAt(const PaneState& state, double width, double height, const PartLimits& limits, RibbonKind kind) {
    return layoutPanes(state, {.width = width, .height = height}, limits, kind);
  }

  void testDefaultLayout() {
    const auto out = layoutAt({}, 1400, 700, kLim, RibbonKind::Flame);
    checkTiles(out, "default");
    checkMinimums(out, kLim, "default");
    TEST_CHECK(rectOf(out.rects.instrument).width == 340);
    TEST_CHECK(rectOf(out.rects.jump).width == 340);
    TEST_CHECK(!sanitizePaneState({}).state.leftWidth.has_value());
    TEST_CHECK(rectOf(out.rects.instrument).height == 434); // 62% of 700, header included
    TEST_CHECK(rectOf(out.rects.jump).height == 266);
    TEST_CHECK(rectOf(out.rects.ribbon).height == 224); // 24 header + the ribbon's preferred 200
    TEST_CHECK(rectOf(out.rects.center).x == 340);
    TEST_CHECK(rectOf(out.rects.center).width == 1060);
    TEST_CHECK(!out.squeezed);
    std::vector<std::pair<DividerId, bool>> dividers;
    dividers.reserve(out.dividers.size());
    for (const auto& divider : out.dividers) {
      dividers.emplace_back(divider.id, divider.draggable);
    }
    TEST_CHECK(
        (dividers
         == std::vector<std::pair<DividerId, bool>>{
             {DividerId::Left, true}, {DividerId::Instrument, true}, {DividerId::Ribbon, true}
         })
    );
  }

  void testDefaultLeftFollowsView() {
    const auto widthAt = [](double width, const PaneState& state) {
      return rectOf(layoutAt(state, width, 800, kLim, RibbonKind::Flame).rects.instrument).width;
    };
    TEST_CHECK(widthAt(1100, {}) == 340);
    TEST_CHECK(widthAt(1400, {}) == 340);
    TEST_CHECK(widthAt(2000, {}) == 440);
    TEST_CHECK(widthAt(2560, {}) == 520);
    TEST_CHECK(widthAt(4000, {}) == 520);
    const auto dragged =
        dragDivider({}, {.width = 2560, .height = 800}, kLim, RibbonKind::Flame, DividerId::Left, 520, -100).state;
    TEST_CHECK(widthAt(2560, dragged) == 420);
    TEST_CHECK(widthAt(1100, dragged) == 420); // a dragged width is kept at every view width
    TEST_CHECK(widthAt(2560, withDividerDefault(dragged, DividerId::Left, RibbonKind::Flame)) == 520);
    TEST_CHECK(defaultLeftWidth(2000) == 440);
    TEST_CHECK(defaultLeftWidth(kNan) == 340);
  }

  void testShortViewRibbonShare() {
    TEST_CHECK(rectOf(layoutAt({}, 1100, 530, kLim, RibbonKind::Flame).rects.ribbon).height == 201);
    TEST_CHECK(rectOf(layoutAt({}, 1100, 900, kLim, RibbonKind::Flame).rects.ribbon).height == 224);
    TEST_CHECK(rectOf(layoutAt({}, 1100, 330, kLim, RibbonKind::Flame).rects.ribbon).height <= 330 - 160);
  }

  void testSmallDefaultViewTiles() {
    for (const auto& [width, height] : std::array<std::pair<double, double>, 4>{{
             {1100, 550},
             {1100, 500},
             {900, 480},
             {800, 450},
         }}) {
      const auto out = layoutAt({}, width, height, kLim, RibbonKind::Flame);
      checkTiles(out, "small default");
      checkMinimums(out, kLim, "small default");
    }
  }

  void testDragLeftClamps() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto far = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, 5000);
    TEST_CHECK(far.value == 1400 - kCenterMinWidth); // the center keeps its minimum, more than the ribbon's 360
    const auto near = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, -5000);
    TEST_CHECK(near.value == 240); // the instrument's minimum width
    for (const auto& drag : {far, near}) {
      const auto out = layoutPanes(drag.state, view, kLim, RibbonKind::Flame);
      checkTiles(out, "after drag");
      checkMinimums(out, kLim, "after drag");
      TEST_CHECK(rectOf(out.rects.instrument).width == drag.value);
    }
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, 60).value == 400);
  }

  void testFlowMinimumLimitsLeft() {
    const auto drag =
        dragDivider({}, {.width = 1400, .height = 700}, kFlow, RibbonKind::Flow, DividerId::Left, 340, 5000);
    TEST_CHECK(drag.value == 1400 - 560);
  }

  void testDragInstrumentDivider() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto down = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Instrument, 434, 5000);
    TEST_CHECK(down.value == 700 - (kPaneHeader + kJumpMinBody));
    const auto up = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Instrument, 434, -5000);
    TEST_CHECK(up.value == kPaneHeader + 220);
    const auto out = layoutPanes(down.state, view, kLim, RibbonKind::Flame);
    TEST_CHECK(rectOf(out.rects.jump).height >= kPaneHeader + kJumpMinBody);
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Instrument, 434, 10).value > 434);
  }

  void testRibbonHeightPerKind() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto taller = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Ribbon, 224, 100);
    TEST_CHECK(taller.value == 324);
    TEST_CHECK(rectOf(layoutPanes(taller.state, view, kLim, RibbonKind::Flame).rects.ribbon).height == 324);
    // the flow ribbon keeps its own default height, 38% of the view
    TEST_CHECK(rectOf(layoutPanes(taller.state, view, kFlow, RibbonKind::Flow).rects.ribbon).height == 266);
    const auto capped = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Ribbon, 224, 5000);
    TEST_CHECK(capped.value == kPaneHeader + 360); // no taller than the part's maximum
    const auto floor = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Ribbon, 224, -5000);
    TEST_CHECK(floor.value == kPaneHeader + 140);
  }

  void testDividerDefaults() {
    const ViewSize view{.width = 1400, .height = 700};
    auto state = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, 100).state;
    state = dragDivider(state, view, kLim, RibbonKind::Flame, DividerId::Ribbon, 224, 40).state;
    state = withDividerDefault(state, DividerId::Left, RibbonKind::Flame);
    const auto out = layoutPanes(state, view, kLim, RibbonKind::Flame);
    TEST_CHECK(rectOf(out.rects.instrument).width == 340);
    TEST_CHECK(rectOf(out.rects.ribbon).height == 264);
    const auto reset = withDividerDefault(state, DividerId::Ribbon, RibbonKind::Flame);
    TEST_CHECK(rectOf(layoutPanes(reset, view, kLim, RibbonKind::Flame).rects.ribbon).height == 224);
    const auto instrument = withDividerValue({}, DividerId::Instrument, 300.6, RibbonKind::Flame);
    TEST_CHECK(instrument.instrumentHeight == 301.0);
    TEST_CHECK(!withDividerDefault(instrument, DividerId::Instrument, RibbonKind::Flame).instrumentHeight);
    const auto ladder = withDividerValue({}, DividerId::Ribbon, 180, RibbonKind::Ladder);
    TEST_CHECK(ladder.ribbonHeights[2] == 180.0);
    TEST_CHECK(!ladder.ribbonHeights[0].has_value());
  }

  void testCollapsing() {
    const ViewSize view{.width = 1400, .height = 700};
    auto out = layoutPanes(togglePane({}, Pane::Instrument), view, kLim, RibbonKind::Flame);
    checkTiles(out, "instrument collapsed");
    TEST_CHECK(rectOf(out.rects.instrument).height == kPaneHeader);
    TEST_CHECK(rectOf(out.rects.jump).height == 700 - kPaneHeader);
    out = layoutPanes(togglePane({}, Pane::Jump), view, kLim, RibbonKind::Flame);
    checkTiles(out, "jump collapsed");
    TEST_CHECK(rectOf(out.rects.jump).height == kPaneHeader);
    TEST_CHECK(rectOf(out.rects.instrument).height == 700 - kPaneHeader);
    out = layoutPanes(togglePane({}, Pane::Ribbon), view, kLim, RibbonKind::Flame);
    checkTiles(out, "ribbon collapsed");
    TEST_CHECK(rectOf(out.rects.ribbon).height == kPaneHeader);
    TEST_CHECK(rectOf(out.rects.center).height == 700 - kPaneHeader);
    TEST_CHECK(!out.forced.ribbon); // a collapse the user asked for is not a forced one
    TEST_CHECK(!togglePane(togglePane({}, Pane::Ribbon), Pane::Ribbon).collapsed.ribbon);
  }

  void testRail() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto state = togglePane(togglePane({}, Pane::Instrument), Pane::Jump);
    const auto out = layoutPanes(state, view, kLim, RibbonKind::Flame);
    checkTiles(out, "rail");
    TEST_CHECK(out.rail);
    TEST_CHECK((out.rects.rail == PaneRect{.x = 0, .y = 0, .width = 32, .height = 700}));
    TEST_CHECK(!out.rects.instrument.has_value());
    TEST_CHECK(rectOf(out.rects.center).x == 32);
    TEST_CHECK(!dividerOf(out, DividerId::Left).draggable);
    const auto back = layoutPanes(togglePane(state, Pane::Jump), view, kLim, RibbonKind::Flame);
    TEST_CHECK(rectOf(back.rects.jump).width == 340); // restoring a pane brings back the saved width
  }

  void testExpanded() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto out = layoutPanes(withExpanded({}, true), view, kLim, RibbonKind::Flame);
    checkTiles(out, "expanded");
    TEST_CHECK((out.rects.instrument == PaneRect{.x = 0, .y = 0, .width = 1400, .height = 700}));
    TEST_CHECK(!out.rects.center.has_value());
    TEST_CHECK(out.expanded);
    TEST_CHECK(out.dividers.empty());
    const auto minimized = layoutPanes(withExpanded({}, false), view, kLim, RibbonKind::Flame);
    TEST_CHECK(out.under.has_value());
    TEST_CHECK(!out.under->instrument.has_value());
    TEST_CHECK(out.under->jump == minimized.rects.jump);
    TEST_CHECK(out.under->ribbon == minimized.rects.ribbon);
    TEST_CHECK(out.under->center == minimized.rects.center);
    TEST_CHECK(out.under->rail == minimized.rects.rail);
    TEST_CHECK(!minimized.under.has_value());
    TEST_CHECK(!out.collapsed.instrument);
    TEST_CHECK(!togglePane(withExpanded({}, true), Pane::Instrument).expanded);
    PaneState wrong;
    wrong.expanded = true;
    wrong.collapsed.instrument = true;
    TEST_CHECK(!sanitizePaneState(wrong).state.expanded);            // a saved state cannot expand a collapsed pane
    TEST_CHECK(sanitizePaneState(wrong).state.collapsed.instrument); // nor does it open the instrument
    TEST_CHECK(sanitizePaneState(wrong).problems.empty());
    const auto reopened = withExpanded(togglePane({}, Pane::Instrument), true);
    TEST_CHECK(!layoutPanes(reopened, view, kLim, RibbonKind::Flame).collapsed.instrument);
  }

  void testNarrowView() {
    auto out = layoutAt({}, 700, 700, kFlow, RibbonKind::Flow);
    checkTiles(out, "700 flow");
    TEST_CHECK(out.forced.ribbon);
    TEST_CHECK(out.collapsed.ribbon);
    TEST_CHECK(rectOf(out.rects.center).width >= kCenterMinWidth);
    TEST_CHECK(out.why.ribbon == ForcedReason::Narrow);
    out = layoutAt({}, 500, 700, kLim, RibbonKind::Flame);
    checkTiles(out, "500");
    TEST_CHECK(out.rail);
    TEST_CHECK(out.forced.instrument);
    TEST_CHECK(out.forced.jump);
  }

  void testShortView() {
    const auto out = layoutAt({}, 1400, 300, kLim, RibbonKind::Flame);
    checkTiles(out, "300");
    TEST_CHECK(out.forced.jump);
    TEST_CHECK(out.forced.ribbon);
    TEST_CHECK(rectOf(out.rects.jump).height == kPaneHeader);
    TEST_CHECK(out.why.ribbon == ForcedReason::Short);
    TEST_CHECK(out.why.jump == ForcedReason::Short);
    TEST_CHECK(layoutAt({}, 500, 700, kLim, RibbonKind::Flame).why.jump == ForcedReason::Narrow);
    TEST_CHECK(layoutAt({}, 1400, 700, kLim, RibbonKind::Flame).why.ribbon == ForcedReason::None);
    TEST_CHECK(layoutAt({}, 700, 250, kFlow, RibbonKind::Flow).why.ribbon == ForcedReason::ShortAndNarrow);
    const auto closedState = togglePane(togglePane({}, Pane::Instrument), Pane::Jump);
    const auto closed = layoutAt(closedState, 300, 300, kLim, RibbonKind::Flame);
    TEST_CHECK(closed.why.instrument == ForcedReason::None); // a pane the user closed has no reason
    TEST_CHECK(closed.why.jump == ForcedReason::None);
    TEST_CHECK(layoutAt({}, 300, 300, kLim, RibbonKind::Flame).why.jump == ForcedReason::Narrow);
    // a forced rail names only the pane the user left open
    const auto oneClosed = layoutAt(togglePane({}, Pane::Instrument), 500, 700, kLim, RibbonKind::Flame);
    TEST_CHECK(oneClosed.rail);
    TEST_CHECK(oneClosed.why.instrument == ForcedReason::None);
    TEST_CHECK(!oneClosed.forced.instrument);
    TEST_CHECK(oneClosed.why.jump == ForcedReason::Narrow);
    TEST_CHECK(oneClosed.forced.jump);
  }

  // Every frame starts from the state and value captured at pointer down, so the divider follows the pointer exactly.
  void testPaneDragFramesFollowPointer() {
    const ViewSize view{.width = 1400, .height = 700};
    std::size_t frames = 0;
    for (const auto id : {DividerId::Left, DividerId::Instrument, DividerId::Ribbon}) {
      for (const double dir : {1.0, -1.0}) {
        const PaneState start;
        const auto layout = layoutPanes(start, view, kLim, RibbonKind::Flame);
        const auto divider = dividerOf(layout, id);
        double previous = divider.value;
        for (std::size_t k = 1; k <= 1200; ++k) { // one pixel per frame covers every divider's whole range
          const double travel = dir * static_cast<double>(k);
          const auto drag = dragDivider(start, view, kLim, RibbonKind::Flame, id, divider.value, travel);
          const double want = std::max(divider.min, std::min(divider.max, divider.value + travel));
          TEST_CHECK(drag.value == want);
          TEST_CHECK(std::abs(drag.value - previous) <= 1);
          TEST_CHECK(dividerOf(layoutPanes(drag.state, view, kLim, RibbonKind::Flame), id).value == drag.value);
          previous = drag.value;
          ++frames;
        }
      }
    }
    TEST_CHECK(frames == 7200);
    // a start value captured earlier wins over where the start state lays the divider out now
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 400, 10).value == 410);
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Ribbon, 300, -20).value == 280);
    // halves round toward positive infinity
    TEST_CHECK(withDividerValue({}, DividerId::Left, 2.5, RibbonKind::Flame).leftWidth == 3.0);
    TEST_CHECK(withDividerValue({}, DividerId::Left, -2.5, RibbonKind::Flame).leftWidth == -2.0);
  }

  void testShrinkKeepsSavedSizes() {
    const auto state =
        dragDivider({}, {.width = 1400, .height = 700}, kLim, RibbonKind::Flame, DividerId::Left, 340, 300).state;
    const auto small = layoutAt(state, 800, 700, kLim, RibbonKind::Flame);
    checkTiles(small, "small");
    TEST_CHECK(rectOf(small.rects.instrument).width == 410);
    TEST_CHECK(rectOf(layoutAt(state, 1400, 700, kLim, RibbonKind::Flame).rects.instrument).width == 640);
    TEST_CHECK(state.leftWidth == 640.0); // layout never rewrites the saved state
    TEST_CHECK(small.state.leftWidth == 640.0);
  }

  std::vector<PaneState> hostileStates() {
    std::vector<PaneState> states;
    const auto add = [&states](auto change) {
      PaneState state;
      change(state);
      states.push_back(state);
    };
    add([](PaneState& s) { s.leftWidth = -5; });
    add([](PaneState& s) { s.leftWidth = kNan; });
    add([](PaneState& s) { s.leftWidth = kInfinity; });
    add([](PaneState& s) { s.instrumentHeight = -1; });
    add([](PaneState& s) { s.instrumentHeight = kNan; });
    add([](PaneState& s) { s.ribbonHeights[0] = -3; });
    add([](PaneState& s) { s.ribbonHeights[1] = kInfinity; });
    add([](PaneState& s) { s.columnWidths = std::array<double, 3>{1, 2, 3}; });
    add([](PaneState& s) { s.columnWidths = std::array<double, 3>{200, 200, kNan}; });
    add([](PaneState& s) { s.columnWidths = std::array<double, 3>{200, 641, 200}; });
    add([](PaneState& s) { s.leftWidth = 1e9; });
    return states;
  }

  void testHostileSavedStates() {
    for (const auto& raw : hostileStates()) {
      const auto sanitized = sanitizePaneState(raw);
      const auto out = layoutAt(raw, 1400, 700, kLim, RibbonKind::Flame);
      checkTiles(out, "hostile state");
      checkMinimums(out, kLim, "hostile state");
      TEST_CHECK(sanitized.problems.size() == 1);
      TEST_CHECK(out.state == sanitized.state);
    }
    TEST_CHECK(sanitizePaneState({}).problems.empty());
    PaneState wide;
    wide.leftWidth = kNan;
    TEST_CHECK(!sanitizePaneState(wide).state.leftWidth.has_value());
    wide.instrumentHeight = 300;
    TEST_CHECK(sanitizePaneState(wide).state.instrumentHeight == 300.0); // a bad field keeps its neighbors
    PaneState columns;
    columns.columnWidths = std::array<double, 3>{200, 190, 150};
    TEST_CHECK((sanitizePaneState(columns).state.columnWidths == std::array<double, 3>{200, 190, 150}));
    columns.columnWidths = std::array<double, 3>{200.4, 190.5, 140};
    TEST_CHECK((sanitizePaneState(columns).state.columnWidths == std::array<double, 3>{200, 191, 140}));
    PaneState huge;
    huge.leftWidth = 1e9;
    TEST_CHECK(sanitizePaneState(huge).problems.size() == 1);
    TEST_CHECK(!sanitizePaneState(huge).state.leftWidth.has_value());
    PaneState edges;
    edges.leftWidth = 0;
    edges.instrumentHeight = kSavedSizeMax;
    TEST_CHECK(sanitizePaneState(edges).problems.empty());
  }

  void testHostileLimits() {
    std::vector<PartLimits> junk(kHostileLimits.begin(), kHostileLimits.end());
    for (const auto& limits : junk) {
      for (const auto& [width, height] :
           std::array<std::pair<double, double>, 4>{{{1400, 700}, {300, 200}, {0, 0}, {2560, 1400}}}) {
        checkTiles(layoutAt({}, width, height, limits, RibbonKind::Flame), "hostile limits");
      }
    }
    for (const auto& view : std::array<ViewSize, 5>{{
             {},
             {.width = kNan, .height = kNan},
             {.width = -5, .height = -5},
             {.width = 1e9, .height = 1e9},
             {.width = kInfinity, .height = -kInfinity},
         }}) {
      const auto out = layoutPanes({}, view, kLim, RibbonKind::Flame);
      TEST_CHECK(out.view.width >= 0 && out.view.height >= 0);
      TEST_CHECK(std::isfinite(out.view.width) && std::isfinite(out.view.height));
    }
    const auto fractional = layoutAt({}, 1400.9, 700.7, kLim, RibbonKind::Flame);
    TEST_CHECK(fractional.view.width == 1400 && fractional.view.height == 700);
  }

  void testNormalizedLimits() {
    TEST_CHECK(
        (normalizeLimits(kHostileLimits[2])
         == PartLimits{
             .instrument = {.minWidth = 240, .minHeight = 220},
             .ribbon = {.minWidth = 0, .minHeight = 120, .preferredHeight = 120, .maxHeight = 120},
         })
    );
    TEST_CHECK(
        (normalizeLimits(kHostileLimits[3])
         == PartLimits{
             .instrument = {.minWidth = 5000, .minHeight = 5000},
             .ribbon = {.minWidth = 5000, .minHeight = 5000, .preferredHeight = 5000, .maxHeight = 5000},
         })
    );
    TEST_CHECK(
        (normalizeLimits(kHostileLimits[4]).ribbon
         == RibbonLimits{.minWidth = 0, .minHeight = 100, .preferredHeight = 100, .maxHeight = 100})
    );
    TEST_CHECK(normalizeLimits({}).ribbon.maxHeight == kInfinity);
    TEST_CHECK(normalizeLimits(kLim) == kLim);
  }

  void testPlanningLimits() {
    TEST_CHECK((kPlanningInstrumentLimits == InstrumentLimits{.minWidth = 240, .minHeight = 220}));
    TEST_CHECK(
        (planningRibbonLimits(RibbonKind::Flame)
         == RibbonLimits{.minWidth = 480, .minHeight = 160, .preferredHeight = 224, .maxHeight = 281})
    );
    TEST_CHECK(
        (planningRibbonLimits(RibbonKind::Flow)
         == RibbonLimits{.minWidth = 560, .minHeight = 180, .preferredHeight = 280, .maxHeight = 420})
    );
    TEST_CHECK(
        (planningRibbonLimits(RibbonKind::Ladder)
         == RibbonLimits{.minWidth = 360, .minHeight = 76, .preferredHeight = 160, .maxHeight = 260})
    );
    TEST_CHECK((planningRibbonLimits(static_cast<RibbonKind>(9)) == planningRibbonLimits(RibbonKind::Flame)));
  }

  void testDragsWithBadInput() {
    const ViewSize view{.width = 1400, .height = 700};
    const auto still = dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, kNan);
    TEST_CHECK(still.value == 340);
    TEST_CHECK(still.state == PaneState{});
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, kInfinity).value == 1010);
    TEST_CHECK(dragDivider({}, view, kLim, RibbonKind::Flame, DividerId::Left, 340, -kInfinity).value == 240);
    // the instrument divider cannot be dragged while Jump ahead is collapsed
    const auto jumpClosed = togglePane({}, Pane::Jump);
    const auto locked = dragDivider(jumpClosed, view, kLim, RibbonKind::Flame, DividerId::Instrument, 434, 5);
    TEST_CHECK(locked.value == 0);
    TEST_CHECK(locked.state == jumpClosed);
    const auto railState = togglePane(jumpClosed, Pane::Instrument);
    TEST_CHECK(dragDivider(railState, view, kLim, RibbonKind::Flame, DividerId::Instrument, 434, 5).value == 0);
    TEST_CHECK(dragDivider(railState, view, kLim, RibbonKind::Flame, DividerId::Left, 32, 5).state == railState);
  }

  void testEmptyView() {
    const auto out = layoutAt({}, 0, 0, kLim, RibbonKind::Flame);
    checkTiles(out, "empty view");
    TEST_CHECK(out.rail);
    TEST_CHECK(out.squeezed);
    TEST_CHECK(out.forced.instrument && out.forced.jump && out.forced.ribbon);
    TEST_CHECK(out.why.ribbon == ForcedReason::Short);
    for (const auto& divider : out.dividers) {
      TEST_CHECK(!divider.draggable);
      TEST_CHECK(divider.length >= 0);
    }
    const auto low = layoutAt(togglePane({}, Pane::Instrument), 1400, 90, kLim, RibbonKind::Flame);
    checkTiles(low, "low view");
    TEST_CHECK(!low.collapsed.jump);
    TEST_CHECK(rectOf(low.rects.jump).height == 90 - kPaneHeader);
    TEST_CHECK(low.squeezed);
    TEST_CHECK(withDividerValue({}, DividerId::Left, kNan, RibbonKind::Flame) == PaneState{});
    TEST_CHECK(withDividerValue({}, DividerId::Left, kInfinity, RibbonKind::Flame) == PaneState{});
  }

  // A small deterministic generator, so a failing case can be rebuilt from its index.
  class Generator {
  public:
    explicit Generator(std::uint32_t seed) : m_state(seed) {}

    double next() {
      m_state = m_state * 1664525U + 1013904223U;
      return static_cast<double>(m_state) / 4294967296.0;
    }

    template <typename T, std::size_t N> const T& pick(const std::array<T, N>& items) {
      return items[static_cast<std::size_t>(std::floor(next() * static_cast<double>(N)))];
    }

  private:
    std::uint32_t m_state;
  };

  void testGenerator() {
    Generator first(20240607);
    TEST_NEAR(first.next(), 0.53243736596778035, 1e-15);
    TEST_NEAR(first.next(), 0.54265549243427813, 1e-15);
    TEST_NEAR(first.next(), 0.86961213964968920, 1e-15);
    Generator second(99);
    TEST_NEAR(second.next(), 0.27443566313013434, 1e-15);
  }

  // A weighted sum of every rect, divider and flag; the expected total is fixed below.
  double digest(const PaneLayout& out) {
    double sum = 0;
    const auto rects = rectList(out.rects);
    for (std::size_t k = 0; k < rects.size(); ++k) {
      if (const auto& r = rects[k]) {
        sum += static_cast<double>(k + 1) * (r->x + 2 * r->y + 3 * r->width + 5 * r->height);
      }
    }
    for (std::size_t k = 0; k < out.dividers.size(); ++k) {
      const auto& d = out.dividers[k];
      sum += static_cast<double>(k + 1) * (d.value + 7 * d.min + 11 * d.max + (d.draggable ? 13 : 0));
    }
    sum += (out.squeezed ? 17 : 0) + (out.rail ? 19 : 0) + (out.expanded ? 23 : 0);
    sum += (out.forced.instrument ? 29 : 0) + (out.forced.jump ? 31 : 0) + (out.forced.ribbon ? 37 : 0);
    return sum;
  }

  constexpr std::array<RibbonKind, 3> kKinds{RibbonKind::Flame, RibbonKind::Flow, RibbonKind::Ladder};

  void testRandomStates() {
    Generator rand(20240607);
    // a non-numeric or missing saved size reads as no saved size
    const std::array<std::optional<double>, 6> odd{kNan, -10.0, std::nullopt, std::nullopt, kInfinity, 1e7};
    const auto maybe = [&](double value) -> std::optional<double> {
      if (rand.next() < 0.15) {
        return rand.pick(odd);
      }
      return value;
    };
    const std::array<PartLimits, 2> usual{kLim, kFlow};
    const std::array<PartLimits, 3> second{kLim, kFlow, kHostileLimits[3]};
    double total = 0;
    std::size_t expandedCases = 0;
    std::size_t hostileCases = 0;
    for (std::size_t i = 0; i < 2000; ++i) {
      PaneState raw;
      raw.leftWidth = maybe(rand.next() * 2500);
      raw.instrumentHeight = rand.next() < 0.4 ? std::nullopt : maybe(rand.next() * 1500);
      raw.collapsed.instrument = rand.next() < 0.2;
      raw.collapsed.jump = rand.next() < 0.2;
      raw.collapsed.ribbon = rand.next() < 0.2;
      raw.expanded = rand.next() < 0.15;
      for (auto& height : raw.ribbonHeights) {
        height = maybe(rand.next() * 800);
      }
      const double width = std::floor(200 + rand.next() * 2800);
      const double height = std::floor(100 + rand.next() * 1400);
      const bool hostile = rand.next() < 0.25;
      const auto limits = hostile ? rand.pick(kHostileLimits) : rand.pick(usual);
      const auto kind = rand.pick(kKinds);
      const auto out = layoutAt(raw, width, height, limits, kind);
      const auto other = layoutAt(raw, width, height, rand.pick(second), kind); // limits change between calls
      checkTiles(other, "second limits");
      checkTiles(out, "random state");
      if (!hostile && width >= 1100 && height >= 520) {
        TEST_CHECK(!out.squeezed);
        checkMinimums(out, limits, "random state");
      }
      const auto again = layoutAt(out.state, width, height, limits, kind);
      TEST_CHECK(again.rects == out.rects); // a layout of a layout's own state does not move
      for (const auto& divider : out.dividers) {
        TEST_CHECK(!divider.draggable || (divider.value >= divider.min && divider.value <= divider.max));
      }
      total += static_cast<double>(i % 97 + 1) * (digest(out) + 3 * digest(other));
      expandedCases += out.expanded ? 1 : 0;
      hostileCases += hostile ? 1 : 0;
    }
    TEST_CHECK(expandedCases == 229);
    TEST_CHECK(hostileCases == 469);
    // the sum, over the 2000 states, of each layout's digest plus three times its other-limits digest, weighted by i %
    // 97 + 1
    TEST_NEAR(total, 29622104534.599773, 1e-4);
  }

  void testRandomDrags() {
    Generator rand(99);
    constexpr std::array<DividerId, 3> kIds{DividerId::Left, DividerId::Instrument, DividerId::Ribbon};
    double total = 0;
    for (std::size_t i = 0; i < 500; ++i) {
      const ViewSize view{
          .width = std::floor(1000 + rand.next() * 1600), .height = std::floor(520 + rand.next() * 900)
      };
      const auto kind = kKinds[i % 3];
      const auto& limits = kind == RibbonKind::Flow ? kFlow : kLim;
      const auto id = kIds[i % 3];
      const auto before = layoutPanes({}, view, limits, kind);
      const auto& start = dividerOf(before, id);
      const auto drag = dragDivider({}, view, limits, kind, id, start.value, std::floor((rand.next() - 0.5) * 4000));
      const auto out = layoutPanes(drag.state, view, limits, kind);
      checkTiles(out, "random drag");
      checkMinimums(out, limits, "random drag");
      TEST_CHECK(dividerOf(out, id).value == drag.value); // the divider sits where the drag put it
      for (const auto& other : out.dividers) {
        if (other.id != id && other.id != DividerId::Instrument && id != DividerId::Left) {
          TEST_CHECK(other.value == dividerOf(before, other.id).value);
        }
      }
      total += static_cast<double>(i % 97 + 1) * (drag.value + digest(out));
    }
    // the sum, over the 500 drags, of the dragged divider's value plus the layout's digest, weighted by i % 97 + 1
    TEST_NEAR(total, 2913810742.0, 1e-6);
  }
} // namespace

int main() {
  testDefaultLayout();
  testDefaultLeftFollowsView();
  testShortViewRibbonShare();
  testSmallDefaultViewTiles();
  testDragLeftClamps();
  testFlowMinimumLimitsLeft();
  testDragInstrumentDivider();
  testRibbonHeightPerKind();
  testDividerDefaults();
  testCollapsing();
  testRail();
  testExpanded();
  testNarrowView();
  testShortView();
  testPaneDragFramesFollowPointer();
  testShrinkKeepsSavedSizes();
  testHostileSavedStates();
  testHostileLimits();
  testNormalizedLimits();
  testPlanningLimits();
  testDragsWithBadInput();
  testEmptyView();
  testGenerator();
  testRandomStates();
  testRandomDrags();
  std::println("workbench pane layout checks passed");
  return 0;
}
