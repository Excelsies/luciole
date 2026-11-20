#include "workbench/atlas_labels.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace luciole::workbench {
  namespace {
    constexpr double kLabelFrom = 25;
    constexpr double kLabelFull = 29;
    constexpr double kLeastAlpha = 0.03;
    constexpr double kSiblingDim = 0.78;
    constexpr double kNoteAlpha = 0.9;
    constexpr double kMetricLeast = 0.05;

    // Strength of the metric line under a centered name; 0 until it passes kMetricLeast.
    double metricStrength(double r) {
      const double strength = fadeRamp(r, 44, 52);
      return strength > kMetricLeast ? strength : 0.0;
    }

    LabelPlan titlePlan(const SceneItem& item, double alpha) {
      const double depth = std::max(12.0, item.r * 0.085);
      return {
          .kind = LabelKind::Title,
          .alpha = alpha,
          .x = item.x,
          .y = item.y - item.r + depth,
          .r = item.r,
          .fontSize = item.r >= 220 ? 14.0 : 13.0,
          .room = std::min(chord(item.r, item.r - depth) - 20, item.r * 1.6),
      };
    }

    // The on-screen part of a circle that is cut by the view edge, or nothing when too little of it shows.
    std::optional<Circle> visiblePart(const SceneItem& item, ViewSize view) {
      const double x0 = std::max(0.0, item.x - item.r);
      const double x1 = std::min(view.width, item.x + item.r);
      const double y0 = std::max(0.0, item.y - item.r);
      const double y1 = std::min(view.height, item.y + item.r);
      if (x1 - x0 >= 2 * item.r - 0.5 && y1 - y0 >= 2 * item.r - 0.5) {
        return Circle{.x = item.x, .y = item.y, .r = item.r};
      }
      const double ax = (x0 + x1) / 2;
      const double ay = (y0 + y1) / 2;
      const double r = std::min({item.r, (x1 - x0) / 2, (y1 - y0) / 2});
      if (x1 <= x0 || y1 <= y0 || std::hypot(ax - item.x, ay - item.y) > item.r - r * 0.6) {
        return std::nullopt;
      }
      return Circle{.x = ax, .y = ay, .r = r};
    }
  } // namespace

  double chord(double r, double dy) { return 2 * std::sqrt(std::max(0.0, r * r - dy * dy)); }

  double labelWidth(double r, double dy) { return std::min(chord(r, dy) * 0.94 - 6, 1.9 * r); }

  LabelPlan labelPlan(const SceneItem& item, ViewSize view) {
    if (item.role == BubbleRole::More) {
      const double alpha = fadeRamp(item.r, kLabelFrom, kLabelFull) * item.fade;
      if (alpha < kLeastAlpha) {
        return {};
      }
      return {
          .kind = LabelKind::More,
          .alpha = alpha,
          .x = item.x,
          .y = item.y,
          .r = item.r,
          .moreLine = chord(item.r, 12) > 40
      };
    }
    if (item.role == BubbleRole::Ancestor || !item.clickable) {
      return {};
    }
    const bool sibling = item.role == BubbleRole::Sibling || item.role == BubbleRole::SiblingChild;
    const double dim = sibling ? kSiblingDim : 1.0;
    const double base = fadeRamp(item.r, kLabelFrom, kLabelFull) * item.fade * dim;
    if (item.role == BubbleRole::Self) {
      if (item.empty) {
        return {
            .kind = LabelKind::EmptyNote,
            .alpha = kNoteAlpha,
            .x = item.x,
            .y = item.y,
            .r = item.r,
            .fontSize = 15,
            .room = chord(item.r, 10) * 0.85,
        };
      }
      return titlePlan(item, 1);
    }
    if (base < kLeastAlpha) {
      return {};
    }
    if (item.kids && item.r >= kAtlasKidsLabeledFrom) {
      return titlePlan(item, item.fade * dim);
    }
    const auto part = visiblePart(item, view);
    if (!part) {
      return {};
    }
    return {
        .kind = LabelKind::Center,
        .alpha = base,
        .x = part->x,
        .y = part->y,
        .r = part->r,
        .fontSize = part->r < 60 ? 12.0
            : part->r >= 140     ? 14.0
                                 : 13.0,
        .metricAlpha = metricStrength(part->r),
        .icon = part->r >= 48,
        .backdrop = item.kids,
    };
  }
} // namespace luciole::workbench
