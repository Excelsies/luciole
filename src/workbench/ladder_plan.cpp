#include "workbench/ladder_plan.h"

#include "workbench/rounding.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace luciole::workbench {
  namespace {
    constexpr std::array<double, kLadderSeparateLevels> kNatural{26, 18, 12, 12, 8, 5, 4, 3};
    constexpr std::array<double, kLadderSeparateLevels> kMinimum{12, 12, 12, 12, 5, 4, 3, 3};
    constexpr std::array<double, kLadderSeparateLevels> kTall{40, 28, 20, 20, 14, 10, 6, 4};
    constexpr std::array<double, 9> kFade{1, 0.92, 0.82, 0.72, 0.62, 0.55, 0.5, 0.45, 0.4};
    constexpr double kMergedH = 3;
    constexpr double kPadTop = 6;
    constexpr double kPadBottom = 3;
    constexpr double kMinW = 360;
    constexpr double kMinH = 76;
    constexpr double kTallestHost = 1e6;
    constexpr std::size_t kGrowRounds = 1000;
    constexpr double kSegmentMin = 3;
    constexpr double kRestCap = 0.15;
    constexpr double kForceMin = 8;
    constexpr double kNearForceMin = 12;
    constexpr std::size_t kShareLookahead = 64;
    constexpr std::size_t kNamedLevels = 3;
    constexpr std::size_t kLabeledLevels = 2;
    constexpr double kLabelMin = 60;
    constexpr double kRestLabelMin = 30;
    constexpr double kMinBarW = 40;

    double cleanShare(double share) { return std::isfinite(share) && share > 0 ? share : 0.0; }

    struct FillPlan {
      std::vector<double> widths;
      std::size_t hidden = 0;
      double tailW = 0;
      double minW = std::numeric_limits<double>::infinity();
      std::optional<LadderForce> tail;
      double width = 0;
    };

    // The first `count` entries split the bar by weight; the rest go to a remainder; a forced tail entry gets its own
    // minimum width after them.
    FillPlan fillPlan(
        std::span<const double> shares, std::size_t entryCount, std::size_t count, double width,
        const LadderSegmentOptions& options, const std::optional<LadderForce>& tail
    ) {
      FillPlan plan;
      plan.tail = tail;
      plan.width = width;
      double shown = 0;
      for (std::size_t i = 0; i < count; ++i) {
        shown += shares[i];
      }
      plan.hidden = entryCount - count - (tail ? 1 : 0);
      const double restFrac = plan.hidden > 0
          ? std::min(options.restCap, std::max(options.min / width, 1 - shown - (tail ? tail->share : 0.0)))
          : 0.0;
      plan.tailW = tail ? options.forceMin : 0.0;
      const double avail = width * (1 - restFrac) - plan.tailW;
      plan.widths.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        plan.widths.push_back(shown > 0 ? shares[i] / shown * avail : avail / static_cast<double>(count));
        plan.minW = std::min(plan.minW, plan.widths.back());
      }
      return plan;
    }

    double sumOf(const std::vector<double>& values) {
      double sum = 0;
      for (const double value : values) {
        sum += value;
      }
      return sum;
    }

    // Gives widths[index] forceMin px, taking the difference from the others in proportion (none under min, the total
    // unchanged). Does nothing when the others cannot give that much.
    void lift(std::vector<double>& widths, std::size_t index, const LadderSegmentOptions& options) {
      const double total = sumOf(widths);
      const double need = options.forceMin - widths[index];
      const double others = total - widths[index];
      if (widths.size() < 2 || others - need < options.min * static_cast<double>(widths.size() - 1)) {
        return;
      }
      const double keep = (others - need) / others;
      for (std::size_t k = 0; k < widths.size(); ++k) {
        if (k != index) {
          widths[k] = std::max(options.min, widths[k] * keep);
        }
      }
      widths[index] = options.forceMin;
      std::size_t big = index == 0 ? 1 : 0;
      for (std::size_t k = 0; k < widths.size(); ++k) {
        if (k != index && widths[k] > widths[big]) {
          big = k;
        }
      }
      widths[big] -= sumOf(widths) - total;
    }

    // Whole-pixel edges that sum to the bar width, after lifting a forced entry that landed under its minimum.
    LadderSegments
    finish(const FillPlan& plan, const LadderSegmentOptions& options, const std::optional<LadderForce>& force) {
      auto widths = plan.widths;
      if (force && force->index < widths.size() && widths[force->index] < options.forceMin) {
        lift(widths, force->index, options);
      }
      LadderSegments result;
      result.segs.reserve(widths.size() + 1);
      double cum = 0;
      const auto place = [&](std::size_t index, double w) {
        result.segs.push_back({.index = index, .x = roundHalfUp(cum), .w = roundHalfUp(cum + w) - roundHalfUp(cum)});
        cum += w;
      };
      for (std::size_t i = 0; i < widths.size(); ++i) {
        place(i, widths[i]);
      }
      if (plan.tail) {
        place(plan.tail->index, plan.tailW);
      }
      if (plan.hidden > 0) {
        const double x = roundHalfUp(cum);
        result.rest = LadderRest{.x = x, .w = std::max(plan.width - x, 1.0), .count = plan.hidden};
      }
      return result;
    }

    // Raises bars[0..count) toward `targets` one pixel at a time, nearest first, round after round, so no bar ever
    // shrinks as `extra` grows. Returns the pixels left over.
    double grow(
        std::vector<LadderBar>& bars, std::size_t count, const std::array<double, kLadderSeparateLevels>& targets,
        double extra
    ) {
      double left = std::max(0.0, extra);
      for (std::size_t round = 0; left > 0 && round < kGrowRounds; ++round) {
        bool gave = false;
        for (std::size_t i = 0; i < count && left > 0; ++i) {
          if (bars[i].h < targets[i]) {
            bars[i].h += 1;
            left -= 1;
            gave = true;
          }
        }
        if (!gave) {
          break;
        }
      }
      return left;
    }

    // Height of the first `separate` bars at their minimum, plus the merged line when levels remain.
    double minimumNeed(std::size_t separate, std::size_t count) {
      double need = kPadTop + kPadBottom;
      for (std::size_t i = 0; i < separate; ++i) {
        need += kMinimum[i] + kLadderGap;
      }
      return need + (separate < count ? kMergedH + kLadderGap : 0.0);
    }

    // Pieces of one bar: the folder's entries, with the on-path entry kept visible even when it is hidden.
    void fillRow(const Tree& tree, const LayoutSettings& settings, LadderRow& row) {
      if (!hasNode(tree, row.folder)) {
        return;
      }
      const auto list = entries(tree, row.folder, settings);
      auto ids = list.ids;
      const bool forced = hasNode(tree, row.onPath);
      const bool hiddenPath = forced && !std::ranges::contains(ids, row.onPath);
      if (hiddenPath) {
        ids.push_back(row.onPath);
      }
      const std::size_t top = std::min(list.ids.size(), kShareLookahead);
      std::vector<double> shares(top, 0.0);
      for (std::size_t i = 0; i < top && list.total > 0; ++i) {
        shares[i] = list.weights[i] / list.total;
      }
      LadderSegmentOptions options;
      if (row.index < kLadderNearLevels) {
        options.forceMin = kNearForceMin;
      }
      if (forced) {
        const double share = list.total > 0 ? weight(tree, row.onPath, settings) / list.total : 0.0;
        const auto index = static_cast<std::size_t>(std::ranges::find(ids, row.onPath) - ids.begin());
        options.force = LadderForce{.index = index, .share = hiddenPath ? std::min(0.5, share) : share};
      }
      const auto plan = ladderSegments(shares, ids.size(), row.w, options);
      const double labelMin = row.index < kLabeledLevels ? kLabelMin : std::numeric_limits<double>::infinity();
      row.pieces.reserve(plan.segs.size());
      for (const auto& seg : plan.segs) {
        const NodeId node = ids[seg.index];
        row.pieces.push_back(
            {.node = node, .x = row.x + seg.x, .w = seg.w, .onPath = node == row.onPath, .labeled = seg.w >= labelMin}
        );
      }
      if (plan.rest) {
        row.rest = LadderRest{.x = row.x + plan.rest->x, .w = plan.rest->w, .count = plan.rest->count};
        row.restLabeled = plan.rest->w >= kRestLabelMin;
      }
    }
  } // namespace

  std::vector<LadderBar> ladderBars(std::size_t count) {
    std::vector<LadderBar> bars;
    const std::size_t separate = std::min(count, kLadderSeparateLevels);
    bars.reserve(separate + 1);
    for (std::size_t i = 0; i < separate; ++i) {
      bars.push_back({.level = i, .firstLevel = i, .h = kNatural[i], .merged = false});
    }
    if (count > kLadderSeparateLevels) {
      bars.push_back({.level = count - 1, .firstLevel = kLadderSeparateLevels, .h = kMergedH, .merged = true});
    }
    return bars;
  }

  double ladderHeight(std::span<const LadderBar> bars) {
    double height = 0;
    for (const auto& bar : bars) {
      height += bar.h;
    }
    return height + (bars.empty() ? 0.0 : static_cast<double>(bars.size() - 1) * kLadderGap);
  }

  double ladderPlanHeight(std::span<const LadderBar> bars) {
    double height = kPadTop + kPadBottom;
    for (const auto& bar : bars) {
      height += bar.h + kLadderGap;
    }
    return height;
  }

  std::vector<LadderBar> ladderPlan(std::size_t count, double height) {
    std::vector<LadderBar> bars;
    if (count == 0) {
      return bars;
    }
    const double host = std::isnan(height) || height < 0 ? 0.0 : std::floor(height);
    const std::size_t near = std::min(count, kLadderNearLevels);
    std::size_t separate = std::min(count, kLadderSeparateLevels);
    // At most four passes. When even the nearest four do not fit, they stay at their minimum and the caller clips.
    while (separate > near && minimumNeed(separate, count) > host) {
      --separate;
    }
    bars.reserve(separate + 1);
    for (std::size_t i = 0; i < separate; ++i) {
      bars.push_back({.level = i, .firstLevel = i, .h = kMinimum[i], .merged = false});
    }
    if (separate < count) {
      bars.push_back({.level = count - 1, .firstLevel = separate, .h = kMergedH, .merged = true});
    }
    if (separate < std::min(count, kLadderSeparateLevels)) {
      return bars;
    }
    const double left = grow(bars, separate, kNatural, host - minimumNeed(separate, count));
    grow(bars, separate, kTall, left);
    return bars;
  }

  LadderLimits ladderLimits(std::size_t count) {
    const double natural = count > 0 ? ladderPlanHeight(ladderBars(count)) : kMinH;
    return {
        .minW = kMinW,
        .minH = kMinH,
        .prefH = std::max(kMinH, natural),
        .maxH = std::max(kMinH, ladderPlanHeight(ladderPlan(count, kTallestHost))),
    };
  }

  LadderSegments ladderSegments(
      std::span<const double> shares, std::size_t entryCount, double width, const LadderSegmentOptions& options
  ) {
    if (entryCount == 0 || !(width > 0) || !std::isfinite(width)) {
      return {};
    }
    LadderSegmentOptions clean = options;
    clean.min = std::isfinite(options.min) && options.min >= 0 ? options.min : kSegmentMin;
    clean.restCap = std::isfinite(options.restCap) ? std::clamp(options.restCap, 0.0, 1.0) : kRestCap;
    if (!(options.forceMin > 0) || !std::isfinite(options.forceMin)) {
      clean.forceMin = kForceMin;
    }
    if (clean.force) {
      if (clean.force->index >= entryCount) {
        clean.force.reset();
      } else {
        clean.force->share = cleanShare(clean.force->share);
      }
    }
    const std::size_t start = std::min({entryCount, options.maxCount, shares.size(), kShareLookahead});
    std::vector<double> cleanShares(start);
    for (std::size_t i = 0; i < start; ++i) {
      cleanShares[i] = cleanShare(shares[i]);
    }
    // One pass per shown entry at most: each pass drops the lightest.
    for (std::size_t count = start;; --count) {
      const auto tail = clean.force && clean.force->index >= count ? clean.force : std::optional<LadderForce>{};
      const auto plan = fillPlan(cleanShares, entryCount, count, width, clean, tail);
      if (plan.minW >= clean.min || count <= 1) {
        return finish(plan, clean, clean.force);
      }
    }
  }

  LadderScene ladderScene(const Tree& tree, const LayoutSettings& settings, double width, double height) {
    LadderScene scene;
    const std::size_t count = tree.ancestors.size();
    scene.atTop = count == 0;
    const double host = std::isfinite(height) && height > 0 ? height : 0.0;
    const auto plan = ladderPlan(count, host);
    const double hostW = std::isfinite(width) ? width : 0.0;
    const double barW = std::max(kMinBarW, hostW - kLadderPadLeft - kLadderPadRight - kLadderGutter);
    double bottom = host - kPadBottom;
    scene.rows.reserve(plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
      const auto& bar = plan[index];
      LadderRow row;
      row.folder = tree.ancestors[bar.level];
      row.onPath = bar.level == 0 ? tree.root : tree.ancestors[bar.level - 1];
      row.index = index;
      row.level = bar.level;
      row.firstLevel = bar.firstLevel;
      row.merged = bar.merged;
      row.named = index < kNamedLevels && !bar.merged;
      row.x = kLadderPadLeft + kLadderGutter;
      row.w = barW;
      row.h = bar.h;
      row.y = bottom - kLadderGap - bar.h;
      row.fade = kFade[std::min(index, kFade.size() - 1)];
      bottom = row.y;
      fillRow(tree, settings, row);
      scene.rows.push_back(std::move(row));
    }
    return scene;
  }
} // namespace luciole::workbench
