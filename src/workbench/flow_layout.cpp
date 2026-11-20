#include "workbench/flow_layout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kEdge = 12.0;
    constexpr double kMinBar = 14.0;
    constexpr double kMinGap = 2.0;
    constexpr double kLineHeight = 15.0;
    constexpr std::size_t kMaxBars = 12;
    constexpr std::size_t kMinBars = 3;
    constexpr std::size_t kFirstMax = 8;

    // Moves boxes apart in order so none overlap and all stay inside lo..hi when they fit.
    void resolveBoxes(std::span<double> tops, std::span<const double> heights, double gap, double lo, double hi) {
      const auto count = tops.size();
      for (std::size_t i = 0; i < count; ++i) {
        tops[i] = std::max(tops[i], i > 0 ? tops[i - 1] + heights[i - 1] + gap : lo);
      }
      for (std::size_t i = count; i > 0; --i) {
        const auto at = i - 1;
        tops[at] = std::min(tops[at], (at + 1 == count ? hi : tops[at + 1] - gap) - heights[at]);
      }
    }

    constexpr double kBarWidth = 16.0;
    constexpr double kMagnify = 2.0;     // a column is scaled at most this much against the previous one
    constexpr double kQuietShare = 0.1;  // More and Files take at most this share of a column
    constexpr double kSparseShare = 0.5; // one or two bars take at most this share of the band
    constexpr double kMaxBarGap = 6.0;
    constexpr double kLabelGap = 6.0;
    constexpr double kTwoLinesHeight = 31.0;
    constexpr double kTwoLinesFrom = 30.0;
    constexpr double kStemJoin = 8.0;
    constexpr double kEmptyBar = 56.0;
    constexpr double kMinColumnStep = kBarWidth + 60.0;
    constexpr double kLastLabelShare = 0.16;
    constexpr double kLastLabelMin = 120.0;
    constexpr double kLastLabelMax = 200.0;

    struct ColumnHeights {
      std::vector<double> heights;
      double scale = 0;
      double gap = 0;
    };

    bool quiet(const FlowRecord& record) {
      return record.kind == FlowRecordKind::More || record.kind == FlowRecordKind::Files;
    }

    // Pixels per unit of weight so that bars of at least `minHeight` fill `available`; 0 when the floors do not fit.
    double solveScale(std::span<const double> weights, double available, double minHeight) {
      const auto count = weights.size();
      if (count == 0 || minHeight * static_cast<double>(count) >= available) {
        return 0.0;
      }
      std::vector<bool> fixed(count, false);
      std::size_t fixedCount = 0;
      double scale = 0;
      // Each pass fixes at least one more bar at its floor or ends.
      for (std::size_t pass = 0; pass <= count; ++pass) {
        double sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
          sum += fixed[i] ? 0.0 : weights[i];
        }
        if (sum <= 0) {
          return 0.0;
        }
        scale = (available - minHeight * static_cast<double>(fixedCount)) / sum;
        bool grew = false;
        for (std::size_t i = 0; i < count; ++i) {
          if (!fixed[i] && weights[i] * scale < minHeight) {
            fixed[i] = true;
            ++fixedCount;
            grew = true;
          }
        }
        if (!grew) {
          break;
        }
      }
      return scale;
    }

    // Weights used for height: a More or Files record never takes more than kQuietShare of its column.
    std::vector<double> effectiveWeights(const FlowTopology& topology, std::span<const std::size_t> column) {
      double real = 0;
      for (const auto index : column) {
        const auto& record = topology.records[index];
        real += quiet(record) ? 0.0 : record.weight;
      }
      const double cap = real * kQuietShare / (1 - kQuietShare);
      std::vector<double> weights;
      weights.reserve(column.size());
      for (const auto index : column) {
        const auto& record = topology.records[index];
        weights.push_back(quiet(record) ? std::min(record.weight, cap > 0 ? cap : record.weight) : record.weight);
      }
      return weights;
    }

    // Bar heights of one column; `maxScale` above 0 limits magnification against the previous column.
    ColumnHeights
    columnHeights(const FlowTopology& topology, std::span<const std::size_t> column, double band, double maxScale) {
      ColumnHeights out;
      const auto count = static_cast<double>(column.size());
      if (column.size() >= 2) {
        const double spare = (band - count * kMinBar) / (count - 1) * 0.5;
        out.gap = std::floor(std::max(kMinGap, std::min(kMaxBarGap, spare)));
      }
      const double gaps = out.gap * (count - 1);
      const auto weights = effectiveWeights(topology, column);
      const double fit = solveScale(weights, band - gaps, kMinBar);
      out.scale = maxScale > 0 ? std::min(fit, maxScale) : fit;
      double sum = 0;
      out.heights.reserve(weights.size());
      for (const auto weight : weights) {
        out.heights.push_back(std::max(kMinBar, weight * out.scale));
        sum += out.heights.back();
      }
      const double room = band * kSparseShare - gaps;
      if (column.size() <= 2 && sum > room) {
        const double factor = std::max(0.0, room) / sum;
        out.scale *= factor;
        for (auto& height : out.heights) {
          height = std::max(kMinBar, height * factor);
        }
      }
      return out;
    }

    // The four bars share the width left after the stem and the last label.
    std::array<double, kFlowColumns> columnXs(double width, double stemWidth, double lastLabel) {
      const double start = kEdge + stemWidth + (stemWidth != 0 ? kStemJoin : 0.0);
      const double span = width - kEdge - lastLabel - start - kBarWidth;
      const double step = std::max(kMinColumnStep, span / 3);
      std::array<double, kFlowColumns> xs{};
      for (std::size_t column = 0; column < kFlowColumns; ++column) {
        xs[column] = std::round(start + static_cast<double>(column) * step);
      }
      return xs;
    }

    // Column 1 stacks its bars centered; the current bar is as tall as the folder and More bars together.
    void placeFirst(FlowTopology& topology, const ColumnHeights& heights, double band, double x0) {
      const auto& column = topology.columns[1];
      double total = heights.gap * static_cast<double>(column.size() - 1);
      double flowing = 0;
      for (const auto index : column) {
        total += topology.records[index].height;
        flowing += topology.records[index].kind == FlowRecordKind::Files ? 0.0 : topology.records[index].height;
      }
      double y = kEdge + (band - total) / 2;
      for (const auto index : column) {
        topology.records[index].y = y;
        y += topology.records[index].height + heights.gap;
      }
      auto& current = topology.records[0];
      current.x = x0;
      current.width = kBarWidth;
      current.height = flowing;
      current.y = kEdge + (band - flowing) / 2;
      double at = current.y;
      for (const auto index : column) {
        auto& record = topology.records[index];
        if (record.kind != FlowRecordKind::Files) {
          record.sliceTop = at;
          record.sliceBottom = at + record.height;
          at += record.height;
        }
      }
    }

    // Slices of a parent bar for its children, centered, squeezed to fit when the children are taller.
    void sliceParent(FlowTopology& topology, std::size_t parent, std::span<const std::size_t> kids) {
      const auto& bar = topology.records[parent];
      double total = 0;
      for (const auto kid : kids) {
        total += topology.records[kid].height;
      }
      const double factor = std::min(1.0, bar.height / std::max(total, 1.0));
      double y = bar.y + (bar.height - total * factor) / 2;
      for (const auto kid : kids) {
        auto& record = topology.records[kid];
        record.sliceTop = y;
        record.sliceBottom = y + record.height * factor;
        y = record.sliceBottom;
      }
    }

    // Columns 2 and 3: each bar sits at its parent's slice, then bars are pushed apart in order; Files at the bottom.
    void placeLater(FlowTopology& topology, std::size_t column, const ColumnHeights& heights, double band) {
      std::vector<std::size_t> flowing;
      std::vector<std::size_t> files;
      std::vector<std::pair<std::size_t, std::vector<std::size_t>>> groups;
      for (const auto index : topology.columns[column]) {
        const auto& record = topology.records[index];
        if (record.kind == FlowRecordKind::Files) {
          files.push_back(index);
          continue;
        }
        flowing.push_back(index);
        auto group = std::ranges::find(groups, record.parent, &std::pair<std::size_t, std::vector<std::size_t>>::first);
        if (group == groups.end()) {
          groups.emplace_back(record.parent, std::vector<std::size_t>{});
          group = std::prev(groups.end());
        }
        group->second.push_back(index);
      }
      for (const auto& [parent, kids] : groups) {
        sliceParent(topology, parent, kids);
      }
      std::vector<std::size_t> all = flowing;
      all.insert(all.end(), files.begin(), files.end());
      std::vector<double> tops;
      std::vector<double> sizes;
      for (const auto index : all) {
        const auto& record = topology.records[index];
        const bool file = record.kind == FlowRecordKind::Files;
        tops.push_back(
            file ? kEdge + band - record.height : (record.sliceTop + record.sliceBottom) / 2 - record.height / 2
        );
        sizes.push_back(record.height);
      }
      resolveBoxes(tops, sizes, heights.gap, kEdge, kEdge + band);
      for (std::size_t i = 0; i < all.size(); ++i) {
        topology.records[all[i]].y = tops[i];
      }
    }

    FlowRibbon ribbonFor(const FlowTopology& topology, std::size_t index) {
      const auto& record = topology.records[index];
      FlowRibbon ribbon;
      ribbon.record = index;
      ribbon.x0 = topology.records[record.parent].x + kBarWidth;
      ribbon.x1 = record.x;
      ribbon.parentTop = record.sliceTop;
      ribbon.parentBottom = record.sliceBottom;
      ribbon.childTop = record.y;
      ribbon.childBottom = record.y + record.height;
      const double middle = (ribbon.x0 + ribbon.x1) / 2;
      ribbon.top = {
          {{ribbon.x0, ribbon.parentTop},
           {middle, ribbon.parentTop},
           {middle, ribbon.childTop},
           {ribbon.x1, ribbon.childTop}}
      };
      ribbon.bottom = {
          {{ribbon.x1, ribbon.childBottom},
           {middle, ribbon.childBottom},
           {middle, ribbon.parentBottom},
           {ribbon.x0, ribbon.parentBottom}}
      };
      ribbon.muted = record.kind == FlowRecordKind::More;
      return ribbon;
    }

    // One label per record, right of its bar, moved apart within its column.
    std::vector<FlowLabel> labelBoxes(const FlowLayout& layout) {
      std::vector<FlowLabel> labels;
      const auto& topology = layout.topology;
      for (std::size_t column = 0; column < kFlowColumns; ++column) {
        const auto& indexes = topology.columns[column];
        std::vector<FlowLabelBox> boxes;
        boxes.reserve(indexes.size());
        for (const auto index : indexes) {
          const auto& record = topology.records[index];
          boxes.push_back(
              {.centerY = record.y + record.height / 2,
               .height = record.height >= kTwoLinesFrom ? kTwoLinesHeight : kLineHeight}
          );
        }
        const auto centers = flowSpreadLabels(boxes, kEdge, layout.height - kEdge);
        const double x = layout.columnX[column] + kBarWidth + kLabelGap;
        const double right = column + 1 < kFlowColumns ? layout.columnX[column + 1] - kLabelGap : layout.width - kEdge;
        for (std::size_t i = 0; i < indexes.size(); ++i) {
          labels.push_back(
              {.record = indexes[i],
               .x = x,
               .centerY = centers[i],
               .height = boxes[i].height,
               .lines = boxes[i].height == kTwoLinesHeight ? 2U : 1U,
               .maxWidth = right - x}
          );
        }
      }
      return labels;
    }

    bool usableSize(double value) { return std::isfinite(value) && value > 0; }

    // Record 0 is the only entry of column 0, every column lists records of that column, and every bar except
    // Files has a parent one column to its left.
    bool consistent(const FlowTopology& topology) {
      const auto& records = topology.records;
      const auto& current = topology.columns[0];
      if (records.empty() || records[0].kind != FlowRecordKind::Current || current.size() != 1 || current[0] != 0) {
        return false;
      }
      for (std::size_t column = 1; column < kFlowColumns; ++column) {
        for (const auto index : topology.columns[column]) {
          if (index >= records.size() || records[index].column != column) {
            return false;
          }
          const auto& record = records[index];
          if (record.kind != FlowRecordKind::Files
              && (record.parent >= records.size() || records[record.parent].column != column - 1)) {
            return false;
          }
        }
      }
      return true;
    }

    constexpr std::array<double, 5> kStemAlpha{1, 0.9, 0.78, 0.66, 0.55};
    constexpr double kStemShare = 0.24;
    constexpr double kStemMin = 150.0;
    constexpr double kStemMax = 250.0;
    constexpr double kPillTextInset = 12.0;
    constexpr double kEmptyLabelWidth = 160.0;

    // Ancestor pills, root first, centered on the current bar, with the links between them.
    void placeStem(FlowLayout& layout, const Tree& tree, FlowStemPlan plan) {
      const auto& current = layout.topology.records[0];
      const double centerY = current.y + current.height / 2;
      const FlowPill* previous = nullptr;
      layout.stem = std::move(plan.pills);
      for (auto& pill : layout.stem) {
        pill.node = tree.ancestors[pill.level - 1];
        pill.x += kEdge;
        pill.y = centerY - pill.height / 2;
        pill.alpha = kStemAlpha[std::min(pill.level - 1, kStemAlpha.size() - 1)];
        pill.textWidth = pill.labeled ? pill.width - kPillTextInset : 0.0;
        if (previous != nullptr) {
          layout.links.push_back({.x0 = previous->x + previous->width, .x1 = pill.x, .centerY = centerY});
        }
        previous = &pill;
      }
      if (previous != nullptr) {
        layout.links.push_back({.x0 = previous->x + previous->width, .x1 = current.x, .centerY = centerY});
      }
    }
  } // namespace

  std::size_t flowCapacity(double height) {
    const double band = (std::isfinite(height) ? height : 0.0) - 2 * kEdge;
    const double perBar = std::max(kMinBar + kMinGap, kLineHeight + 1);
    const double fit = std::floor((band + 1) / perBar);
    return static_cast<std::size_t>(
        std::max(static_cast<double>(kMinBars), std::min(static_cast<double>(kMaxBars), fit))
    );
  }

  FlowOptions flowOptions(double height) {
    const auto capacity = flowCapacity(height);
    return {.perColumn = capacity, .firstMax = std::clamp<std::size_t>(capacity - 2, 1, kFirstMax)};
  }

  FlowLayout placeFlow(FlowTopology topology, double width, double height, double stemWidth) {
    FlowLayout layout;
    if (!usableSize(width)
        || !usableSize(height)
        || !std::isfinite(stemWidth)
        || stemWidth < 0
        || !consistent(topology)) {
      return layout;
    }
    layout.width = width;
    layout.height = height;
    layout.topology = std::move(topology);
    auto& placed = layout.topology;
    const double band = height - 2 * kEdge;
    const double lastLabel = std::max(kLastLabelMin, std::min(kLastLabelMax, width * kLastLabelShare));
    layout.columnX = columnXs(width, stemWidth, lastLabel);
    double previousScale = 0;
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      const auto& indexes = placed.columns[column];
      if (indexes.empty()) {
        continue;
      }
      const auto heights = columnHeights(placed, indexes, band, previousScale > 0 ? previousScale * kMagnify : 0.0);
      if (heights.scale > 0) {
        previousScale = heights.scale;
      }
      for (std::size_t i = 0; i < indexes.size(); ++i) {
        auto& record = placed.records[indexes[i]];
        record.x = layout.columnX[column];
        record.width = kBarWidth;
        record.height = heights.heights[i];
      }
      if (column == 1) {
        placeFirst(placed, heights, band, layout.columnX[0]);
      } else {
        placeLater(placed, column, heights, band);
      }
    }
    if (placed.columns[1].empty()) {
      auto& current = placed.records[0];
      current.x = layout.columnX[0];
      current.width = kBarWidth;
      current.height = kEmptyBar;
      current.y = kEdge + (band - kEmptyBar) / 2;
    }
    for (std::size_t column = 1; column < kFlowColumns; ++column) {
      for (const auto index : placed.columns[column]) {
        if (placed.records[index].kind != FlowRecordKind::Files) {
          layout.ribbons.push_back(ribbonFor(placed, index));
        }
      }
    }
    layout.labels = labelBoxes(layout);
    return layout;
  }

  FlowLayout flowLayout(const Tree& tree, const LayoutSettings& settings, double width, double height) {
    if (!usableSize(width) || !usableSize(height) || !hasNode(tree, tree.root)) {
      return {};
    }
    auto plan = flowStemPlan(tree.ancestors.size(), std::max(kStemMin, std::min(kStemMax, width * kStemShare)));
    auto layout =
        placeFlow(buildFlowTopology(tree, tree.root, settings, flowOptions(height)), width, height, plan.width);
    placeStem(layout, tree, std::move(plan));
    if (layout.topology.empty != FlowEmpty::None) {
      const auto& current = layout.topology.records[0];
      layout.labels = {
          {.record = 0,
           .x = current.x + kBarWidth + kLabelGap,
           .centerY = current.y + current.height / 2,
           .height = kTwoLinesHeight,
           .lines = 2,
           .maxWidth = kEmptyLabelWidth}
      };
    }
    return layout;
  }

  std::vector<double> flowSpreadLabels(std::span<const FlowLabelBox> boxes, double lo, double hi) {
    std::vector<double> tops;
    std::vector<double> heights;
    tops.reserve(boxes.size());
    heights.reserve(boxes.size());
    for (const auto& box : boxes) {
      tops.push_back(box.centerY - box.height / 2);
      heights.push_back(box.height);
    }
    resolveBoxes(tops, heights, 1.0, lo, hi);
    for (std::size_t i = 0; i < tops.size(); ++i) {
      tops[i] += heights[i] / 2;
    }
    return tops;
  }
} // namespace luciole::workbench
