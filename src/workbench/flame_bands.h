#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The rows of the flame graph band, top to bottom: the far line, the ancestor strata, the bar and the strata ahead.
namespace luciole::workbench {
  // Shortest band the ribbon works at: four ancestor strata, the bar and two strata ahead at their floors.
  inline constexpr double kFlameMinRibbon = 158;
  // Shortest band that draws the three ways out beside the far line with the strata ahead at full height.
  inline constexpr double kFlameMinBand = 224;
  // Tallest useful band: six ancestor strata, the far line, the bar and the strata ahead at their cap.
  inline constexpr double kFlameMaxBand = 281;

  struct FlameLimits {
    double minW = 0;
    double minH = 0;
    double prefH = 0;
    double maxH = 0;
  };

  inline constexpr FlameLimits kFlameLimits{.minW = 480, .minH = 160, .prefH = kFlameMinBand, .maxH = kFlameMaxBand};

  enum class FlameRowKind : std::uint8_t { Far, Ancestor, Bar, Ahead };

  struct FlameRow {
    FlameRowKind kind = FlameRowKind::Bar;
    std::size_t level = 0; // far line: strata + 1; ancestor stratum k; bar 0; stratum ahead 1 to 3
    double y = 0;
    double h = 0;
  };

  struct FlameBands {
    std::vector<FlameRow> rows; // top to bottom
    std::size_t strata = 0;     // ancestor strata drawn
    bool far = false;           // the far line is drawn
    double scale = 1;           // growth of the strata ahead
    std::size_t ahead = 0;      // strata ahead drawn
    double bottom = 0;          // bottom edge of the last row
  };

  // Rows of a band `height` px tall for a folder with `ancestorCount` ancestors. What gets height, in order: the four
  // nearest ancestor strata and the bar, two strata ahead at their floors, the third, the strata ahead at full height,
  // the far line and strata 5 and 6, then spare height grows the strata ahead (the first at most 64 px).
  FlameBands flameBands(double height, std::size_t ancestorCount);

  // Band height in a host: the host's, within 158 and 281 px. The band sits against the host's bottom edge.
  double flameBandHeight(double hostHeight);

  // The row under y: a gap belongs to the nearer row, the padding to none.
  std::optional<std::size_t> flameRowAt(std::span<const FlameRow> rows, double y);
} // namespace luciole::workbench
