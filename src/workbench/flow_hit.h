#pragma once

#include "workbench/flow_layout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// What lies under a point of a placed flow diagram: pills, bars and labels; ribbons take no hits.
namespace luciole::workbench {
  enum class FlowHitKind : std::uint8_t { Stem, Record };

  struct FlowHit {
    FlowHitKind kind = FlowHitKind::Record;
    std::size_t index = 0; // into FlowLayout::stem or FlowTopology::records
    bool operator==(const FlowHit&) const = default;
  };

  // The stem pill or record under a point: bars are padded to 18 px tall, pills to 16 px; a label counts when
  // its drawn width is given in `labelWidths` (aligned with FlowLayout::labels). The nearest center wins.
  std::optional<FlowHit> flowHitAt(const FlowLayout& layout, std::span<const double> labelWidths, double x, double y);
} // namespace luciole::workbench
