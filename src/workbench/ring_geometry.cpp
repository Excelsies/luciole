#include "workbench/ring_geometry.h"

#include "workbench/rounding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace luciole::workbench::ring {
  namespace {
    constexpr std::size_t kFixingPasses = 6;

    double cleanWeight(double weight) { return std::isfinite(weight) && weight > 0 ? weight : 0.0; }

    // Clean weights and their sum.
    std::pair<std::vector<double>, double> cleanWeights(std::span<const double> weights) {
      std::vector<double> clean;
      clean.reserve(weights.size());
      double sum = 0;
      for (const auto weight : weights) {
        clean.push_back(cleanWeight(weight));
        sum += clean.back();
      }
      return {std::move(clean), sum};
    }

    bool hostFits(double width, double height) {
      return std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0;
    }

    double ringsWidth(std::span<const double> thickness, double gap) {
      double total = 0;
      for (const auto ring : thickness) {
        total += ring + gap;
      }
      return total;
    }

    // At least two rows; the value is bounded by the clamped host size.
    std::size_t rowCount(double rows) { return static_cast<std::size_t>(std::max(2.0, rows)); }
  } // namespace

  double wrapDegrees(double degrees) { return std::fmod(std::fmod(degrees, 360.0) + 360.0, 360.0); }

  std::vector<double> arcAngles(std::span<const double> weights, double minDegrees, double total) {
    const auto count = weights.size();
    if (count == 0) {
      return {};
    }
    if (!std::isfinite(total) || total <= 0) {
      return std::vector<double>(count, 0.0);
    }
    const auto [clean, sum] = cleanWeights(weights);
    auto equal = std::vector<double>(count, total / static_cast<double>(count));
    if (!(sum > 0) || !std::isfinite(sum) || static_cast<double>(count) * minDegrees >= total) {
      return equal;
    }
    std::vector<bool> fixed(count, false);
    double free = total;
    double freeWeight = sum;
    for (std::size_t pass = 0; pass < kFixingPasses; ++pass) {
      bool changed = false;
      for (std::size_t i = 0; i < count; ++i) {
        if (!fixed[i] && (clean[i] / freeWeight) * free < minDegrees) {
          fixed[i] = true;
          free -= minDegrees;
          freeWeight -= clean[i];
          changed = true;
        }
      }
      if (!changed) {
        break;
      }
    }
    if (free <= 0 || freeWeight <= 0) {
      return equal;
    }
    std::vector<double> angles;
    angles.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      angles.push_back(fixed[i] ? minDegrees : (clean[i] / freeWeight) * free);
    }
    return angles;
  }

  std::vector<double> arcStarts(std::span<const double> angles, double from) {
    std::vector<double> starts;
    starts.reserve(angles.size());
    double angle = from;
    for (const auto span : angles) {
      starts.push_back(angle);
      angle += span;
    }
    return starts;
  }

  ArcFit
  fitCount(std::span<const double> weights, std::size_t rowsFit, double minDegrees, double moreDegrees, double slack) {
    const auto count = weights.size();
    if (count == 0) {
      return {};
    }
    // More rows than entries change nothing, and the cap keeps 2 * rowsFit from wrapping.
    const std::size_t columnRows = std::min(rowsFit, count);
    const bool more = count > 2 * columnRows;
    const std::size_t first = more ? (columnRows > 0 ? 2 * columnRows - 1 : 0) : count;
    // One try per shown count, fewest last.
    for (std::size_t shown = first; shown >= 1; --shown) {
      const bool hasMore = more || shown < count;
      auto angles = arcAngles(weights.first(shown), minDegrees, hasMore ? 360.0 - moreDegrees : 360.0);
      if (hasMore) {
        angles.push_back(moreDegrees);
      }
      const auto rows = static_cast<long long>(angles.size());
      const auto starts = arcStarts(angles);
      long long before = 0;
      long long within = 0;
      for (std::size_t i = 0; i < angles.size(); ++i) {
        const double mid = starts[i] + angles[i] / 2;
        before += mid < 180.0 - slack ? 1 : 0;
        within += mid <= 180.0 + slack ? 1 : 0;
      }
      const auto fit = static_cast<long long>(columnRows);
      const long long low = std::max(rows - fit, before);
      const long long high = std::min(fit, within);
      if (low > high) {
        continue;
      }
      const double half = static_cast<double>(rows) / 2;
      long long split = low;
      for (long long k = low; k <= high; ++k) {
        if (std::abs(static_cast<double>(k) - half) < std::abs(static_cast<double>(split) - half)) {
          split = k;
        }
      }
      return {.shown = shown, .more = hasMore, .angles = std::move(angles), .split = static_cast<std::size_t>(split)};
    }
    if (count > 1) {
      return {.shown = 1, .more = true, .angles = {360.0 - moreDegrees, moreDegrees}, .split = 1};
    }
    return {.shown = 1, .more = false, .angles = {360.0}, .split = 1};
  }

  ArcFit fitCompact(std::span<const double> weights, std::size_t count, double minDegrees, double moreDegrees) {
    const auto total = weights.size();
    if (total == 0) {
      return {};
    }
    const auto wanted = std::max<std::size_t>(count, 1);
    const bool more = total > wanted;
    const auto shown = more ? wanted : total;
    auto angles = arcAngles(weights.first(shown), minDegrees, more ? 360.0 - moreDegrees : 360.0);
    if (more) {
      angles.push_back(moreDegrees);
    }
    const auto split = angles.size();
    return {.shown = shown, .more = more, .angles = std::move(angles), .split = split};
  }

  RingSplit ringSegments(std::span<const double> weights, std::size_t pathIndex, double mergeDegrees) {
    if (pathIndex >= weights.size()) {
      return {};
    }
    const auto [clean, sum] = cleanWeights(weights);
    if (!(sum > 0) || !std::isfinite(sum)) {
      return {};
    }
    std::vector<std::size_t> keep;
    RingSplit split;
    for (std::size_t i = 0; i < clean.size(); ++i) {
      if ((clean[i] / sum) * 360.0 >= mergeDegrees || i == pathIndex) {
        keep.push_back(i);
      } else {
        split.merged.push_back(i);
      }
    }
    std::vector<double> kept;
    kept.reserve(keep.size() + 1);
    std::size_t pathSlot = 0;
    for (std::size_t j = 0; j < keep.size(); ++j) {
      kept.push_back(clean[keep[j]]);
      pathSlot = keep[j] == pathIndex ? j : pathSlot;
    }
    if (!split.merged.empty()) {
      double rest = 0;
      for (const auto i : split.merged) {
        rest += clean[i];
      }
      kept.push_back(rest);
    }
    const auto angles = arcAngles(kept, mergeDegrees);
    const auto starts = arcStarts(angles);
    const double shift = -(starts[pathSlot] + angles[pathSlot] / 2);
    split.segments.reserve(kept.size());
    for (std::size_t j = 0; j < kept.size(); ++j) {
      const bool merged = j == keep.size();
      split.segments.push_back(
          {.entry = merged ? 0 : keep[j],
           .merged = merged,
           .arc = {.a0 = starts[j] + shift, .a1 = starts[j] + angles[j] + shift}}
      );
    }
    return split;
  }

  std::vector<RingSegment> compose(std::span<const double> weights, Arc span, double minDegrees) {
    const double width = span.a1 - span.a0;
    if (!(width > 0) || !std::isfinite(width)) {
      return {};
    }
    const auto [clean, sum] = cleanWeights(weights);
    if (!(sum > 0) || !std::isfinite(sum)) {
      return {};
    }
    std::vector<std::size_t> keep;
    std::vector<double> kept;
    double rest = 0;
    for (std::size_t i = 0; i < clean.size(); ++i) {
      if ((clean[i] / sum) * width >= minDegrees) {
        keep.push_back(i);
        kept.push_back(clean[i]);
      } else {
        rest += clean[i];
      }
    }
    if (rest > 0) {
      kept.push_back(rest);
    }
    const auto slots = static_cast<double>(std::max<std::size_t>(1, kept.size()));
    const auto angles = arcAngles(kept, std::min(minDegrees, width / slots), width);
    const auto starts = arcStarts(angles, span.a0);
    std::vector<RingSegment> pieces;
    pieces.reserve(kept.size());
    for (std::size_t j = 0; j < kept.size(); ++j) {
      const bool merged = j >= keep.size();
      pieces.push_back(
          {.entry = merged ? 0 : keep[j], .merged = merged, .arc = {.a0 = starts[j], .a1 = starts[j] + angles[j]}}
      );
    }
    return pieces;
  }

  std::optional<Dial> fullDial(double width, double height) {
    if (!hostFits(width, height)) {
      return std::nullopt;
    }
    Dial dial;
    dial.width = std::min(width, kMaxHostSize);
    dial.height = std::min(height, kMaxHostSize);
    dial.thickness.assign(kContextThickness.begin(), kContextThickness.end());
    dial.gap = kContextGap;
    dial.lane = 18.0;
    dial.stackRows = kStackRows;
    dial.stackRow = kStackRow;
    const double rings = ringsWidth(dial.thickness, dial.gap);
    const double baseGap = 6.0;
    const double margin = baseGap + dial.lane + 2.0; // leaders reach rTotal + margin
    const double top = 10.0 + kStackRow * static_cast<double>(kStackRows) + margin;
    const double bottom = margin + 4.0;
    const double minColumn = 260.0;
    const double edge = 20.0 + dial.lane;
    dial.rTotal =
        std::max(std::min((dial.height - top - bottom) / 2, (dial.width - 2 * (minColumn + edge + 12.0)) / 2), 110.0);
    dial.center = {.x = dial.width / 2, .y = top + dial.rTotal};
    dial.rOut = dial.rTotal - rings;
    dial.rIn = roundHalfUp(dial.rOut * 0.5);
    dial.rHub = dial.rIn - 8.0;
    dial.rBase = dial.rTotal + baseGap;
    dial.right = {.x0 = dial.center.x + dial.rTotal + edge, .x1 = dial.width - 12.0};
    dial.left = {.x0 = 12.0, .x1 = dial.center.x - dial.rTotal - edge};
    dial.yMin = dial.center.y - dial.rBase + 12.0;
    dial.yMax = dial.center.y + dial.rBase - 12.0;
    dial.rowsFit = rowCount(std::floor((dial.yMax - dial.yMin) / kRowHeight) + 1);
    dial.stackBottom = dial.center.y - dial.rTotal - margin;
    return dial;
  }

  std::optional<Dial> compactDial(double width, double height, std::size_t rings) {
    if (!hostFits(width, height)) {
      return std::nullopt;
    }
    Dial dial;
    dial.width = std::min(width, kMaxHostSize);
    dial.height = std::min(height, kMaxHostSize);
    dial.compact = true;
    dial.thickness.assign(kCompactThickness.begin(), kCompactThickness.begin() + std::min(rings, kContextRings));
    dial.gap = kCompactGap;
    dial.stackRows = dial.height >= 330.0 ? 3 : 0;
    dial.stackRow = kCompactStackRow;
    dial.readoutHeight = kReadoutHeight;
    const double top = 6.0 + static_cast<double>(dial.stackRows) * kCompactStackRow + (dial.stackRows > 0 ? 4.0 : 0.0);
    const double bottom = 4.0 + kReadoutHeight;
    const double body = dial.height - top - bottom;
    const double half = body / 2;
    dial.legend = dial.width - 2 * half - 8.0 - 8.0 - 12.0 >= 170.0;
    dial.rTotal = std::max(60.0, std::min(half, dial.legend ? half : (dial.width - 16.0) / 2));
    dial.center = {.x = dial.legend ? 8.0 + dial.rTotal : dial.width / 2, .y = top + half};
    dial.rOut = dial.rTotal - ringsWidth(dial.thickness, dial.gap);
    dial.rIn = roundHalfUp(dial.rOut * 0.46);
    dial.rHub = dial.rIn - 5.0;
    dial.rBase = dial.rTotal + 4.0;
    dial.right = {.x0 = dial.center.x + dial.rTotal + 12.0, .x1 = dial.width - 8.0};
    dial.left = {.x0 = 8.0, .x1 = 8.0};
    dial.rowsFit = rowCount(std::floor(body / kRowHeight));
    const auto centered = static_cast<double>(std::min<std::size_t>(8, dial.rowsFit) - 1);
    dial.yMin = dial.center.y - centered * kRowHeight / 2;
    dial.yMax = dial.height - bottom - 12.0;
    dial.stackBottom = dial.center.y - dial.rTotal - 3.0;
    return dial;
  }

  bool chooseCompact(double width, double height, std::optional<bool> wasCompact) {
    const bool below = width < 860.0 || height < 460.0;
    if (!wasCompact.has_value() || !*wasCompact) {
      return below;
    }
    const bool roomy = width >= 900.0 && height >= 490.0;
    return !roomy;
  }

  std::vector<Band> ringBands(double rOut, std::span<const double> thickness, double gap) {
    std::vector<Band> bands;
    bands.reserve(thickness.size());
    double radius = rOut + gap;
    for (const auto ring : thickness) {
      bands.push_back({.r0 = radius, .r1 = radius + ring});
      radius += ring + gap;
    }
    return bands;
  }

  Point polar(Point center, double radius, double degrees) {
    return {.x = center.x + radius * std::sin(degrees * kRadian), .y = center.y - radius * std::cos(degrees * kRadian)};
  }

  bool inSector(Point center, Arc arc, Band band, Point point, double pad) {
    const double dx = point.x - center.x;
    const double dy = point.y - center.y;
    const double radius = std::hypot(dx, dy);
    if (radius < band.r0 + pad || radius > band.r1 - pad) {
      return false;
    }
    const double cut = (pad / radius) / kRadian;
    const double offset = wrapDegrees(std::atan2(dx, -dy) / kRadian - arc.a0);
    return offset >= cut && offset <= arc.a1 - arc.a0 - cut;
  }

  std::optional<Point> labelSpot(const Dial& dial, Arc arc, double width, double height) {
    constexpr std::array kOffsets{0.0, -0.18, 0.18, -0.3, 0.3};
    constexpr std::array<Point, 8> kProbes{{{-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
    const double middle = (arc.a0 + arc.a1) / 2;
    const double radius = (dial.rIn + dial.rOut) / 2;
    const double thickness = dial.rOut - dial.rIn;
    const Band band{.r0 = dial.rIn, .r1 = dial.rOut};
    for (const auto offset : kOffsets) {
      const auto spot = polar(dial.center, radius + offset * thickness, middle);
      const bool fits = std::ranges::all_of(kProbes, [&](Point probe) {
        return inSector(
            dial.center, arc, band, {.x = spot.x + probe.x * width / 2, .y = spot.y + probe.y * height / 2}
        );
      });
      if (fits) {
        return spot;
      }
    }
    return std::nullopt;
  }

  std::vector<double> placeRows(std::span<const double> ideal, double rowHeight, double yMin, double yMax) {
    struct Block {
      double sum = 0;
      std::size_t count = 0;
    };
    std::vector<Block> blocks;
    blocks.reserve(ideal.size());
    for (std::size_t i = 0; i < ideal.size(); ++i) {
      blocks.push_back({.sum = ideal[i] - static_cast<double>(i) * rowHeight, .count = 1});
      // Each merge removes a block, so this runs at most i times.
      while (blocks.size() > 1) {
        const auto last = blocks.back();
        auto& before = blocks[blocks.size() - 2];
        if (before.sum / static_cast<double>(before.count) <= last.sum / static_cast<double>(last.count)) {
          break;
        }
        before = {.sum = before.sum + last.sum, .count = before.count + last.count};
        blocks.pop_back();
      }
    }
    const double low = yMin;
    const double high = yMax - static_cast<double>(ideal.empty() ? 0 : ideal.size() - 1) * rowHeight;
    std::vector<double> rows;
    rows.reserve(ideal.size());
    for (const auto& block : blocks) {
      const double y = std::min(high, std::max(low, block.sum / static_cast<double>(block.count)));
      for (std::size_t k = 0; k < block.count; ++k) {
        rows.push_back(y + static_cast<double>(rows.size()) * rowHeight);
      }
    }
    return rows;
  }

  bool arcHolds(Arc arc, double origin, double degrees) {
    const double offset = wrapDegrees(degrees - origin);
    return offset >= arc.a0 - origin && offset < arc.a1 - origin;
  }

  std::optional<std::size_t> locate(std::span<const Arc> arcs, double degrees) {
    if (arcs.empty()) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < arcs.size(); ++i) {
      if (arcHolds(arcs[i], arcs.front().a0, degrees)) {
        return i;
      }
    }
    return std::nullopt;
  }

  double rowIdeal(const Dial& dial, double degrees) { return dial.center.y - dial.rBase * std::cos(degrees * kRadian); }

} // namespace luciole::workbench::ring
