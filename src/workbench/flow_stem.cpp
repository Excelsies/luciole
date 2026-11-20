#include "workbench/flow_stem.h"

#include <algorithm>
#include <array>

namespace luciole::workbench {
  namespace {
    constexpr std::array<double, 5> kPillHeight{28, 22, 16, 12, 8};
    constexpr std::array<double, 5> kPillWidth{86, 70, 26, 20, 12};
    constexpr std::array<double, 3> kPillMinWidth{46, 42, 14};
    constexpr double kFarPillMinWidth = 5.0;
    constexpr double kDot = 8.0;
    constexpr double kDotMinWidth = 6.0;
    constexpr std::size_t kDotFromLevels = 4;
    constexpr std::size_t kLabeledLevels = 2;
    constexpr std::size_t kNearLevels = 3;
    constexpr double kStemGap = 2.0;
    constexpr double kTightStemGap = 1.0;

    double stemTotal(const std::vector<FlowPill>& pills, double gap) {
      double total = 0;
      for (const auto& pill : pills) {
        total += pill.width;
      }
      return total + gap * static_cast<double>(pills.size() - 1);
    }
  } // namespace

  FlowStemPlan flowStemPlan(std::size_t depth, double width) {
    FlowStemPlan plan;
    const std::size_t levels = std::min(depth, kMaxFlowStemLevels);
    if (levels == 0) {
      return plan;
    }
    plan.pills.reserve(levels);
    for (std::size_t level = levels; level >= 1; --level) {
      const bool dot = level == levels && levels >= kDotFromLevels;
      const auto row = std::min<std::size_t>(level - 1, kPillHeight.size() - 1);
      FlowPill pill;
      pill.level = level;
      pill.kind = dot ? FlowPillKind::Dot : FlowPillKind::Pill;
      pill.labeled = level <= kLabeledLevels;
      pill.height = dot ? kDot : kPillHeight[row];
      pill.width = dot ? kDot : kPillWidth[row];
      pill.minWidth =
          dot ? kDotMinWidth : (level <= kPillMinWidth.size() ? kPillMinWidth[level - 1] : kFarPillMinWidth);
      plan.pills.push_back(pill);
    }
    double gap = kStemGap;
    double total = stemTotal(plan.pills, gap);
    if (total > width) {
      double room = 0;
      for (const auto& pill : plan.pills) {
        room += pill.width - pill.minWidth;
      }
      const double cut = std::min(1.0, (total - width) / std::max(room, 1.0));
      for (auto& pill : plan.pills) {
        pill.width -= (pill.width - pill.minWidth) * cut;
      }
      total = stemTotal(plan.pills, gap);
    }
    if (total > width) {
      gap = kTightStemGap;
      double nearWidth = 0;
      std::size_t far = 0;
      for (const auto& pill : plan.pills) {
        if (pill.level <= kNearLevels) {
          nearWidth += pill.width;
        } else {
          ++far;
        }
      }
      const double room = width - gap * static_cast<double>(plan.pills.size() - 1) - nearWidth;
      for (auto& pill : plan.pills) {
        if (pill.level > kNearLevels) {
          pill.width = std::max(1.0, std::min(pill.width, room / static_cast<double>(far)));
        }
      }
    }
    double x = 0;
    for (auto& pill : plan.pills) {
      pill.x = x;
      x += pill.width + gap;
    }
    plan.width = std::max(0.0, x - gap);
    plan.gap = gap;
    return plan;
  }
} // namespace luciole::workbench
