#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// One row of the flame graph: a span divided among entries by weight, the smallest merged into one piece.
namespace luciole::workbench {
  inline constexpr std::size_t kNoFlameItem = std::numeric_limits<std::size_t>::max();
  // The share of a row the merged piece takes at most, unless told otherwise.
  inline constexpr double kFlameRowCap = 0.15;

  // Advance widths of one label font in px: one per ASCII code point, one for every other code point.
  struct LabelFont {
    std::array<double, 128> ascii{};
    double other = 0;
  };

  // The label fonts: first stratum ahead 13 px semibold, other strata ahead 12 px medium, ancestor strata 12 px
  // semibold, and the values beside or under the names ahead 11 px regular.
  struct FlameFonts {
    LabelFont firstAhead;
    LabelFont ahead;
    LabelFont ancestor;
    LabelFont value;
  };

  struct FlameSegment {
    std::size_t index = 0; // entry index
    double x = 0;
    double w = 0;
  };

  // Entries merged into one piece at the end of a row.
  struct FlameMore {
    double x = 0;
    double w = 0;
    std::vector<std::size_t> indices;
  };

  struct FlameRowSplit {
    std::vector<FlameSegment> segs; // in entry order
    std::optional<FlameMore> more;
  };

  struct FlameRowOptions {
    double min = 0;                 // narrowest segment
    double cap = kFlameRowCap;      // the merged piece takes at most this much of the span, but at least `min`
    std::size_t pin = kNoFlameItem; // entry that stays shown however small
    double pinMin = 0;              // width the pinned entry keeps
    std::span<const double> want;   // label width per entry, heaviest first; empty means a plain split
  };

  double labelTextWidth(std::string_view text, const LabelFont& font);

  // Divides [x0, x1] among entries whose `shares` sum to 1, heaviest first. Entries too small for `min` merge into
  // one piece at the end; the shown entries divide what is left by weight. The last piece ends exactly at x1.
  FlameRowSplit flameRowLayout(std::span<const double> shares, double x0, double x1, const FlameRowOptions& options);
} // namespace luciole::workbench
