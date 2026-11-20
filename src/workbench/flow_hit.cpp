#include "workbench/flow_hit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace luciole::workbench {
  namespace {
    struct Box {
      double x = 0;
      double y = 0;
      double width = 0;
      double height = 0;
    };

    bool inside(const Box& box, double x, double y) {
      return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
    }

    // A pill answers to its own width plus 1 px each side and at least kStemHitHeight.
    constexpr double kStemHitHeight = 16.0;
    // A bar answers to kBarPad px each side and at least kBarHitHeight.
    constexpr double kBarPad = 4.0;
    constexpr double kBarHitHeight = 18.0;
    constexpr double kLabelPad = 6.0;

    Box stemBox(const FlowPill& pill) {
      const double height = std::max(pill.height, kStemHitHeight);
      return {.x = pill.x - 1, .y = pill.y + pill.height / 2 - height / 2, .width = pill.width + 2, .height = height};
    }

    Box barBox(const FlowRecord& record) {
      const double height = std::max(record.height, kBarHitHeight);
      return {
          .x = record.x - kBarPad,
          .y = record.y + record.height / 2 - height / 2,
          .width = record.width + 2 * kBarPad,
          .height = height,
      };
    }
  } // namespace

  std::optional<FlowHit> flowHitAt(const FlowLayout& layout, std::span<const double> labelWidths, double x, double y) {
    std::optional<FlowHit> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    const auto consider = [&](FlowHit hit, bool hitHere, double centerY) {
      const double distance = std::abs(y - centerY);
      if (hitHere && distance < bestDistance) {
        best = hit;
        bestDistance = distance;
      }
    };
    for (std::size_t i = 0; i < layout.stem.size(); ++i) {
      const auto& pill = layout.stem[i];
      consider({.kind = FlowHitKind::Stem, .index = i}, inside(stemBox(pill), x, y), pill.y + pill.height / 2);
    }
    const auto& records = layout.topology.records;
    std::vector<std::size_t> labelOf(records.size(), kNoRecord);
    for (std::size_t i = 0; i < layout.labels.size(); ++i) {
      if (layout.labels[i].record < records.size()) {
        labelOf[layout.labels[i].record] = i;
      }
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto& record = records[i];
      bool hit = inside(barBox(record), x, y);
      const auto label = labelOf[i];
      if (!hit && label < labelWidths.size() && labelWidths[label] > 0) {
        const auto& box = layout.labels[label];
        hit = inside(
            {.x = box.x - kLabelPad,
             .y = box.centerY - box.height / 2,
             .width = labelWidths[label] + kLabelPad + 2,
             .height = box.height},
            x, y
        );
      }
      consider({.kind = FlowHitKind::Record, .index = i}, hit, record.y + record.height / 2);
    }
    return best;
  }
} // namespace luciole::workbench
