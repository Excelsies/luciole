#include "workbench/atlas_label_shapes.h"

#include "workbench/atlas_labels.h"
#include "workbench/atlas_text.h"
#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>

namespace luciole::workbench {
  using namespace scene_style;

  namespace {
    constexpr double kMetricFont = 11;
    constexpr double kRowPad = 4;   // a row is its font size plus this
    constexpr double kIconRow = 18; // room above the name for the icon
    constexpr double kIconSize = 16;
    constexpr double kIconInk = 0.95;   // the icon's share of the label's strength
    constexpr double kBlockFill = 0.92; // a label block may fill this share of the radius
    constexpr double kPillPad = 16;     // pill width beyond the text
    constexpr double kPillGrow = 8;     // pill height beyond the block
    constexpr double kPillIconWidth = 28;
    constexpr double kPillRadius = 9;
    constexpr double kChipHeight = 22;
    constexpr double kChipDot = 3.5;
    constexpr double kChipDotAt = 11; // the dot's center from the chip's left
    constexpr double kChipText = 20;  // the name's start from the chip's left
    constexpr double kChipLead = 22;  // dot and gap before the name
    constexpr double kChipTail = 12;  // room after the text
    constexpr double kChipMetricGap = 8;
    constexpr double kMoreCountFont = 13;
    constexpr double kMoreCountRise = 6;
    constexpr double kMoreLineDrop = 9;
    // Measured widths add up each character's advance at one size; drawn text runs a few percent wider, so a centered
    // box leaves that room, and a chip's text box reaches to what follows it.
    constexpr double kTextGrowth = 0.08;
    constexpr double kTextSlack = 2;

    Rgba8 withAlpha(Rgba8 color, double alpha) {
      color.a = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * color.a));
      return color;
    }

    // One line of measured text centered on x, ranked by the circle's area.
    ShapeLabel* centered(
        Builder& b, NodeId node, const std::string& text, double x, double middle, Rgb ink, double alpha, double area,
        double width, double size
    ) {
      const double box = std::max(width * (1 + kTextGrowth) + kTextSlack, kAtlasLeastTextRoom);
      auto* label = b.label(node, text, x - box / 2, middle, box, ink, area, LabelAlign::Center, size);
      if (label != nullptr) {
        label->color = withAlpha(label->color, alpha);
      }
      return label;
    }

    void moreLabel(Builder& b, const SceneItem& item, const LabelPlan& plan, const LabelFont& font) {
      const double area = item.r * item.r;
      const auto count = std::format("+{}", countText(static_cast<double>(item.restCount)));
      const auto& p = b.palette;
      const double y = plan.y - kMoreCountRise;
      if (auto* label = centered(
              b, kNoNode, count, plan.x, y, p.onSurface, plan.alpha, area, atlasTextWidth(count, font, kMoreCountFont),
              kMoreCountFont
          )) {
        label->fit = LabelFit::Whole;
        label->weight = LabelWeight::Semibold;
      }
      if (plan.moreLine) {
        const std::string more = "more";
        if (auto* label = centered(
                b, kNoNode, more, plan.x, plan.y + kMoreLineDrop, p.onSurfaceVariant, plan.alpha, area,
                atlasTextWidth(more, font, kMetricFont), kMetricFont
            )) {
          label->fit = LabelFit::Whole;
        }
      }
    }

    void titleChip(
        Builder& b, const LayoutSettings& settings, const SceneItem& item, const LabelPlan& plan,
        const NodeColor& color, const LabelFont& font, RankedShapes& under
    ) {
      const double size = plan.fontSize;
      const auto name = fitAtlasText(nodeName(b.tree, item.node), plan.room - kChipLead, font, size);
      if (name.empty()) {
        return;
      }
      const double nameWidth = atlasTextWidth(name, font, size);
      const auto metric = measureLabel(b.tree, item.node, settings.sizeBy);
      const double metricRoom = atlasTextWidth(metric, font, kMetricFont) + kChipMetricGap;
      const bool withMetric = nameWidth + kChipLead + metricRoom + kChipTail <= plan.room;
      const double width = nameWidth + kChipLead + (withMetric ? metricRoom : 0) + kChipTail;
      const double left = plan.x - width / 2;
      const double area = item.r * item.r;
      const auto id = sid(item.node, SceneRole::Detail);
      auto chip = rectShape(id, left, plan.y - kChipHeight / 2, width, kChipHeight, kPillRadius);
      chip.fill = tone(b.palette.surface, kHudChipFill);
      chip.opacity = static_cast<float>(plan.alpha);
      chip.hittable = false;
      under.emplace_back(area, chip);
      auto dot = circleShape(id, {.x = left + kChipDotAt, .y = plan.y}, kChipDot);
      dot.fill = opaque(color.accent);
      dot.opacity = static_cast<float>(plan.alpha);
      dot.hittable = false;
      under.emplace_back(area, dot);
      const double middle = plan.y + 0.5;
      if (auto* label = b.label(
              item.node, name, left + kChipText, middle,
              std::max(nameWidth + (withMetric ? kChipMetricGap : kChipTail), kAtlasLeastTextRoom), b.palette.onSurface,
              area, LabelAlign::Start, size
          )) {
        label->color = withAlpha(label->color, plan.alpha);
        label->weight = LabelWeight::Semibold;
      }
      if (withMetric) {
        if (auto* label = b.label(
                item.node, metric, left + kChipText + nameWidth + kChipMetricGap, middle,
                std::max(metricRoom - kChipMetricGap + kChipTail, kAtlasLeastTextRoom), b.palette.onSurfaceVariant,
                area, LabelAlign::Start, kMetricFont
            )) {
          label->color = withAlpha(label->color, plan.alpha);
          label->fit = LabelFit::Whole;
        }
      }
    }

    void centerLabel(
        Builder& b, const LayoutSettings& settings, const SceneItem& item, const LabelPlan& plan,
        const NodeColor& color, const LabelFont& font, RankedShapes& under
    ) {
      const double size = plan.fontSize;
      const double iconRow = plan.icon ? kIconRow : 0;
      const double rowHeight = size + kRowPad;
      double metricRow = plan.metricAlpha > 0 ? kMetricFont + kRowPad : 0;
      const auto rows =
          atlasNameRows(nodeName(b.tree, item.node), plan.r, iconRow + rowHeight + metricRow, rowHeight, font, size);
      if (rows.empty()) {
        return;
      }
      double total = iconRow + rowHeight * static_cast<double>(rows.size()) + metricRow;
      const auto widths = rows
          | std::views::transform([&](const std::string& row) { return atlasTextWidth(row, font, size); })
          | std::ranges::to<std::vector>();
      const double nameWidth = std::ranges::max(widths);
      // The measure shows whole within the circle; on a pill only at full strength, the pill then wide enough for it.
      const auto metric = measureLabel(b.tree, item.node, settings.sizeBy);
      const double metricWidth = atlasTextWidth(metric, font, kMetricFont);
      if (metricRow > 0 && (metricWidth > labelWidth(plan.r, total / 2) || (plan.backdrop && plan.metricAlpha < 1))) {
        total -= metricRow;
        metricRow = 0;
      }
      const double textRoom =
          std::max({nameWidth, plan.icon ? kPillIconWidth : 0.0, metricRow > 0 ? metricWidth : 0.0});
      if (total / 2 > plan.r * kBlockFill) {
        return;
      }
      const double top = plan.y - total / 2;
      const double area = item.r * item.r;
      const bool folder = hasNode(b.tree, item.node) && b.tree.nodes[item.node].folder;
      if (plan.backdrop) {
        const double width = textRoom + kPillPad;
        const double height = total + kPillGrow;
        auto pill = rectShape(
            sid(item.node, SceneRole::Detail), plan.x - width / 2, plan.y - height / 2, width, height,
            std::min(kPillRadius, height / 2)
        );
        pill.fill = tone(b.palette.surface, kHudChipFill);
        pill.opacity = static_cast<float>(plan.alpha);
        pill.hittable = false;
        under.emplace_back(area, pill);
      }
      const Rgb ink = plan.backdrop || folder ? b.palette.onSurface : color.ink;
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const double middle = top + iconRow + rowHeight * (static_cast<double>(i) + 0.5);
        auto* label = centered(b, item.node, rows[i], plan.x, middle, ink, plan.alpha, area, widths[i], size);
        if (label != nullptr) {
          label->weight = LabelWeight::Semibold;
        }
        if (label != nullptr && i == 0 && plan.icon && hasNode(b.tree, item.node)) {
          label->icon = {
              .category = static_cast<FileCategory>(b.tree.nodes[item.node].category),
              .size = static_cast<float>(kIconSize),
              .left = static_cast<float>((label->width - kIconSize) / 2),
              .top = static_cast<float>(top + (kIconRow - kIconSize) / 2 - label->y),
              .color = tone(folder ? color.accent : color.ink, kIconInk * plan.alpha),
          };
        }
      }
      if (metricRow > 0) {
        const double middle = top + iconRow + rowHeight * static_cast<double>(rows.size()) + metricRow / 2 - 1;
        if (auto* label = centered(
                b, item.node, metric, plan.x, middle, b.palette.onSurfaceVariant, plan.alpha * plan.metricAlpha, area,
                metricWidth, kMetricFont
            )) {
          label->fit = LabelFit::Whole;
        }
      }
    }
  } // namespace

  void addAtlasLabel(
      Builder& b, const LayoutSettings& settings, const SceneItem& item, ViewSize view, const NodeColor& color,
      const LabelFont& font, RankedShapes& under
  ) {
    const auto plan = labelPlan(item, view);
    switch (plan.kind) {
    case LabelKind::More:
      moreLabel(b, item, plan, font);
      return;
    case LabelKind::Title:
      titleChip(b, settings, item, plan, color, font, under);
      return;
    case LabelKind::Center:
      centerLabel(b, settings, item, plan, color, font, under);
      return;
    case LabelKind::None:
    case LabelKind::EmptyNote:
      return;
    }
  }

  void putKeptLabelShapes(Builder& b, const RankedShapes& under) {
    double least = 0;
    if (b.labels.size() > kMaxSceneLabels) {
      std::vector<double> areas;
      areas.reserve(b.labels.size());
      for (const auto& [area, label] : b.labels) {
        areas.push_back(area);
      }
      const auto cut = areas.begin() + static_cast<std::ptrdiff_t>(kMaxSceneLabels - 1);
      std::ranges::nth_element(areas, cut, std::ranges::greater{});
      least = *cut;
    }
    for (const auto& [area, shape] : under) {
      if (area >= least) {
        b.put(shape);
      }
    }
  }
} // namespace luciole::workbench
