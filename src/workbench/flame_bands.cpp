#include "workbench/flame_bands.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace luciole::workbench {
  namespace {
    constexpr double kPad = 8;
    constexpr double kGap = 1;
    constexpr double kGroupGap = 4;
    constexpr double kFarH = 4;
    constexpr double kBarH = 26;
    constexpr std::array<double, 6> kAncestorH{24, 16, 12, 12, 6, 4};
    constexpr std::array<double, 3> kAheadH{44, 34, 24};
    constexpr std::array<double, 3> kAheadMin{24, 18, 14};
    constexpr double kAheadSum = 44 + 34 + 24;
    constexpr double kScaleMax = 64.0 / 44.0 + 1e-9; // the first stratum ahead never grows past 64 px
    constexpr double kGrowFrom = 16;                 // spare height that starts growing the strata ahead
    constexpr std::size_t kNearStrata = 4;

    double ancestorNeed(std::size_t strata, bool far, std::size_t depth) {
      if (depth == 0) {
        return 0.0;
      }
      double sum = 0;
      for (std::size_t i = 0; i < strata; ++i) {
        sum += kAncestorH[i];
      }
      return sum + static_cast<double>(strata) * kGap + (far ? kFarH + kGap : 0.0) + kGroupGap;
    }

    double heightSum(std::span<const double> heights) {
      double sum = 0;
      for (const double height : heights) {
        sum += height;
      }
      return sum;
    }

    double aheadNeed(std::span<const double> heights) {
      return heightSum(heights) + static_cast<double>(heights.size()) * kGap;
    }
  } // namespace

  FlameBands flameBands(double height, std::size_t ancestorCount) {
    const double host = std::isnan(height) || height < 0 ? 0.0 : height;
    const double avail = host - 2 * kPad;
    const std::size_t nearest = std::min(ancestorCount, kNearStrata);
    const double fixed = ancestorNeed(nearest, false, ancestorCount) + kBarH;
    std::size_t ahead = kAheadMin.size();
    while (ahead > 1 && fixed + aheadNeed(std::span(kAheadMin).first(ahead)) > avail) {
      --ahead;
    }
    const auto low = std::span(kAheadMin).first(ahead);
    const auto high = std::span(kAheadH).first(ahead);
    const double raise = heightSum(high) - heightSum(low);
    const double budget = std::max(0.0, avail - fixed - aheadNeed(low));
    FlameBands bands;
    bands.strata = nearest;
    bands.ahead = ahead;
    std::vector<double> heights(high.begin(), high.end());
    if (budget < raise) {
      for (std::size_t i = 0; i < ahead; ++i) {
        heights[i] = low[i] + std::floor((high[i] - low[i]) * budget / raise);
      }
    } else {
      double left = budget - raise;
      for (std::size_t more = std::min(ancestorCount, kAncestorH.size()); more > nearest; --more) {
        const double extra =
            ancestorNeed(more, ancestorCount > more, ancestorCount) - ancestorNeed(nearest, false, ancestorCount);
        if (extra <= left) {
          bands.strata = more;
          bands.far = ancestorCount > more;
          left -= extra;
          break;
        }
      }
      if (bands.strata == nearest && ancestorCount > nearest && kFarH + kGap <= left) {
        bands.far = true;
        left -= kFarH + kGap;
      }
      if (ahead == kAheadH.size() && left >= kGrowFrom) {
        bands.scale = std::min(kScaleMax, 1 + left / kAheadSum);
        for (std::size_t i = 0; i < ahead; ++i) {
          heights[i] = std::floor(kAheadH[i] * bands.scale);
        }
      }
    }
    double y = kPad;
    if (bands.far) {
      bands.rows.push_back({.kind = FlameRowKind::Far, .level = bands.strata + 1, .y = y, .h = kFarH});
      y += kFarH + kGap;
    }
    for (std::size_t k = bands.strata; k > 0; --k) {
      bands.rows.push_back({.kind = FlameRowKind::Ancestor, .level = k, .y = y, .h = kAncestorH[k - 1]});
      y += kAncestorH[k - 1] + kGap;
    }
    if (ancestorCount > 0) {
      y += kGroupGap - kGap;
    }
    bands.rows.push_back({.kind = FlameRowKind::Bar, .level = 0, .y = y, .h = kBarH});
    y += kBarH + kGap;
    for (std::size_t i = 0; i < ahead; ++i) {
      bands.rows.push_back({.kind = FlameRowKind::Ahead, .level = i + 1, .y = y, .h = heights[i]});
      y += heights[i] + kGap;
    }
    bands.bottom = y - kGap;
    return bands;
  }

  double flameBandHeight(double hostHeight) {
    return std::isnan(hostHeight) ? kFlameMinRibbon : std::max(kFlameMinRibbon, std::min(kFlameMaxBand, hostHeight));
  }

  std::optional<std::size_t> flameRowAt(std::span<const FlameRow> rows, double y) {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& row = rows[i];
      const double end = row.y + row.h;
      const double top = i > 0 ? (rows[i - 1].y + rows[i - 1].h + row.y) / 2 : row.y;
      const double bottom = i + 1 < rows.size() ? (end + rows[i + 1].y) / 2 : end;
      if (y >= top && y < bottom) {
        return i;
      }
    }
    return std::nullopt;
  }
} // namespace luciole::workbench
