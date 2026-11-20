#include "workbench/treemap.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kHalfPi = std::numbers::pi / 2.0;
    constexpr std::size_t kMaxArcSteps = 64;
    constexpr double kEndGuard = 1e-9;

    bool finiteArea(const TreemapRect& rect) {
      return std::isfinite(rect.x)
          && std::isfinite(rect.y)
          && std::isfinite(rect.w)
          && std::isfinite(rect.h)
          && rect.w > 0
          && rect.h > 0;
    }

    BandPoint arcAt(const Band& band, double centerX, double centerY, double angle) {
      return {
          .x = centerX + band.cornerRadius * std::cos(angle),
          .y = centerY + band.cornerRadius * std::sin(angle),
          .normalX = std::cos(angle),
          .normalY = std::sin(angle),
      };
    }

    // Lengths of the item segments by water filling: items under their floor get the floor and leave the pool, the
    // rest share what remains by raw length. Each pass removes at least one item or ends.
    void fillLengths(
        std::vector<double>& lengths, std::vector<bool>& flexible, const std::vector<double>& raw,
        const std::vector<double>& floors, double length, double fixed
    ) {
      const std::size_t count = raw.size();
      for (std::size_t pass = 0; pass <= count + 1; ++pass) {
        double flexRaw = 0;
        for (std::size_t i = 0; i < count; ++i) {
          if (flexible[i]) {
            flexRaw += raw[i];
          }
        }
        const double factor = flexRaw > 0 ? (length - fixed) / flexRaw : 0.0;
        bool anyLow = false;
        for (std::size_t i = 0; i < count; ++i) {
          if (flexible[i] && raw[i] * factor < floors[i]) {
            anyLow = true;
          }
        }
        if (!anyLow) {
          for (std::size_t i = 0; i < count; ++i) {
            if (flexible[i]) {
              lengths[i] = raw[i] * factor;
            }
          }
          return;
        }
        for (std::size_t i = 0; i < count; ++i) {
          if (flexible[i] && raw[i] * factor < floors[i]) {
            lengths[i] = floors[i];
            fixed += lengths[i];
            flexible[i] = false;
          }
        }
      }
    }
  } // namespace

  std::vector<TreemapRect> squarify(std::span<const double> weights, TreemapRect rect) {
    const std::size_t count = weights.size();
    if (count == 0 || !finiteArea(rect)) {
      return {};
    }
    double total = 0;
    for (const auto value : weights) {
      if (!(value > 0) || !std::isfinite(value)) {
        return {};
      }
      total += value;
    }
    const double rectArea = rect.w * rect.h;
    if (!std::isfinite(total) || !std::isfinite(rectArea) || !(rectArea > 0)) {
      return {};
    }
    const double scale = rectArea / total;
    if (!std::isfinite(scale) || !(scale > 0)) {
      return {};
    }
    std::vector<TreemapRect> out(count);
    double x = rect.x;
    double y = rect.y;
    double w = rect.w;
    double h = rect.h;
    std::size_t i = 0;
    // Every pass lays at least one weight, so count + 1 passes always finish.
    for (std::size_t pass = 0; i < count && pass <= count; ++pass) {
      const double side = std::min(w, h);
      const double side2 = side * side;
      std::size_t j = i;
      double sum = 0;
      double low = std::numeric_limits<double>::infinity();
      double high = 0;
      double worst = std::numeric_limits<double>::infinity();
      for (; j < count; ++j) {
        const double area = weights[j] * scale;
        const double s = sum + area;
        const double mn = std::min(low, area);
        const double mx = std::max(high, area);
        const double ratio = std::max((side2 * mx) / (s * s), (s * s) / (side2 * mn));
        if (j > i && ratio > worst) {
          break;
        }
        sum = s;
        low = mn;
        high = mx;
        worst = ratio;
      }
      const bool wide = w >= h;
      const bool last = j >= count;
      const double thick = last ? (wide ? w : h) : sum / side;
      double pos = wide ? y : x;
      for (std::size_t k = i; k < j; ++k) {
        const double len = k == j - 1 ? (wide ? y + h : x + w) - pos : (weights[k] * scale) / thick;
        out[k] = wide ? TreemapRect{.x = x, .y = pos, .w = thick, .h = len}
                      : TreemapRect{.x = pos, .y = y, .w = len, .h = thick};
        pos += len;
      }
      if (wide) {
        x += thick;
        w -= thick;
      } else {
        y += thick;
        h -= thick;
      }
      i = j;
    }
    return out;
  }

  std::optional<Band> band(TreemapRect rect, double thickness, double outerRadius) {
    if (!finiteArea(rect) || !std::isfinite(thickness) || !std::isfinite(outerRadius) || thickness <= 0) {
      return std::nullopt;
    }
    Band g;
    g.rect = rect;
    g.thickness = thickness;
    g.cornerRadius =
        std::max(0.0, std::min(outerRadius - thickness / 2.0, (std::min(rect.w, rect.h) - thickness) / 2.0));
    g.halfStraightX = std::max(0.0, (rect.w - thickness) / 2.0 - g.cornerRadius);
    g.halfStraightY = std::max(0.0, (rect.h - thickness) / 2.0 - g.cornerRadius);
    g.centerX = rect.x + rect.w / 2.0;
    g.centerY = rect.y + rect.h / 2.0;
    const double arc = kHalfPi * g.cornerRadius;
    g.lengths = {2 * g.halfStraightX, arc, 2 * g.halfStraightY, arc,
                 2 * g.halfStraightX, arc, 2 * g.halfStraightY, arc};
    double position = 0;
    for (std::size_t i = 0; i < kBandPieces; ++i) {
      g.starts[i] = position;
      position += g.lengths[i];
    }
    g.perimeter = position;
    return g;
  }

  std::size_t bandPiece(const Band& band, double s) {
    std::size_t i = kBandPieces - 1;
    while (i > 0 && s < band.starts[i]) {
      --i;
    }
    return i;
  }

  BandPoint pointAt(const Band& band, double s) {
    const double length = band.perimeter;
    const double wrapped = length > 0 && std::isfinite(s) ? std::fmod(std::fmod(s, length) + length, length) : 0.0;
    const std::size_t piece = bandPiece(band, wrapped);
    const double u = wrapped - band.starts[piece];
    const double angle = band.cornerRadius > 0 ? u / band.cornerRadius : 0.0;
    const double cx = band.centerX;
    const double cy = band.centerY;
    const double sx = band.halfStraightX;
    const double sy = band.halfStraightY;
    const double rc = band.cornerRadius;
    switch (piece) {
    case 0:
      return {.x = cx - sx + u, .y = cy - sy - rc, .normalX = 0, .normalY = -1};
    case 1:
      return arcAt(band, cx + sx, cy - sy, -kHalfPi + angle);
    case 2:
      return {.x = cx + sx + rc, .y = cy - sy + u, .normalX = 1, .normalY = 0};
    case 3:
      return arcAt(band, cx + sx, cy + sy, angle);
    case 4:
      return {.x = cx + sx - u, .y = cy + sy + rc, .normalX = 0, .normalY = 1};
    case 5:
      return arcAt(band, cx - sx, cy + sy, kHalfPi + angle);
    case 6:
      return {.x = cx - sx - rc, .y = cy + sy - u, .normalX = -1, .normalY = 0};
    default:
      return arcAt(band, cx - sx, cy - sy, std::numbers::pi + angle);
    }
  }

  std::optional<double> locate(const Band& band, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || !(band.perimeter > 0)) {
      return std::nullopt;
    }
    const double qx = x - band.centerX;
    const double qy = y - band.centerY;
    const double sx = band.halfStraightX;
    const double sy = band.halfStraightY;
    const double rc = band.cornerRadius;
    const auto& starts = band.starts;
    double s = 0;
    if (std::abs(qx) <= sx) {
      if (qy < -sy) {
        s = starts[0] + qx + sx;
      } else if (qy > sy) {
        s = starts[4] + sx - qx;
      } else {
        return std::nullopt;
      }
    } else if (std::abs(qy) <= sy) {
      s = qx > 0 ? starts[2] + qy + sy : starts[6] + sy - qy;
    } else if (qx > 0 && qy < 0) {
      s = starts[1] + (std::atan2(qy + sy, qx - sx) + kHalfPi) * rc;
    } else if (qx > 0) {
      s = starts[3] + std::atan2(qy - sy, qx - sx) * rc;
    } else if (qy > 0) {
      s = starts[5] + (std::atan2(qy - sy, qx + sx) - kHalfPi) * rc;
    } else {
      s = starts[7] + (std::atan2(qy + sy, qx + sx) + std::numbers::pi) * rc;
    }
    s = std::min(std::max(s, 0.0), band.perimeter - kEndGuard);
    const auto p = pointAt(band, s);
    const double offset = (x - p.x) * p.normalX + (y - p.y) * p.normalY;
    if (std::abs(offset) <= band.thickness / 2.0) {
      return s;
    }
    return std::nullopt;
  }

  std::vector<TreemapPoint> segmentPolygon(const Band& band, double s0, double s1) {
    if (!std::isfinite(s0) || !std::isfinite(s1) || s1 <= s0 || band.perimeter <= 0) {
      return {};
    }
    std::vector<double> cuts{s0};
    const double step = std::max(band.cornerRadius * 0.2, 0.25);
    for (std::size_t i = 0; i < kBandPieces; ++i) {
      const double a = std::max(s0, band.starts[i]);
      const double b = std::min(s1, band.starts[i] + band.lengths[i]);
      if (b <= a) {
        continue;
      }
      if (i % 2 == 1 && band.cornerRadius > 0) {
        double s = a + step;
        for (std::size_t k = 0; s < b - 1e-6 && k < kMaxArcSteps; ++k) {
          cuts.push_back(s);
          s += step;
        }
      } else {
        if (a > s0) {
          cuts.push_back(a);
        }
        if (b < s1) {
          cuts.push_back(b);
        }
      }
    }
    cuts.push_back(s1);
    std::ranges::sort(cuts);
    const double half = band.thickness / 2.0;
    std::vector<TreemapPoint> outline;
    outline.reserve(cuts.size() * 2);
    for (const auto s : cuts) {
      const auto p = pointAt(band, std::min(s, band.perimeter - kEndGuard));
      outline.push_back({.x = p.x + p.normalX * half, .y = p.y + p.normalY * half});
    }
    for (const auto s : std::views::reverse(cuts)) {
      const auto p = pointAt(band, std::min(s, band.perimeter - kEndGuard));
      outline.push_back({.x = p.x - p.normalX * half, .y = p.y - p.normalY * half});
    }
    return outline;
  }

  std::vector<BandSegment> partition(std::span<const BandItem> items, double length, const PartitionOptions& options) {
    const std::size_t count = items.size();
    if (count == 0 || !(length > 0) || !std::isfinite(length)) {
      return {};
    }
    const double minLength = options.minLength;
    const auto pinned = [&options](const BandItem& item) {
      return options.pinKey.has_value() && item.key == *options.pinKey;
    };
    double total = 0;
    for (const auto& item : items) {
      total += item.share;
    }
    std::vector<double> raw(count);
    std::vector<double> floors(count);
    for (std::size_t i = 0; i < count; ++i) {
      raw[i] = total > 0 ? (items[i].share / total) * length : length / static_cast<double>(count);
      floors[i] = pinned(items[i]) ? std::max(options.pinMinimum, minLength) : minLength;
    }
    std::vector<bool> flexible(count);
    std::vector<std::size_t> small;
    for (std::size_t i = 0; i < count; ++i) {
      flexible[i] = raw[i] >= floors[i];
      if (!flexible[i]) {
        small.push_back(i);
      }
    }
    if (small.size() == 1 && !pinned(items[small.front()])) {
      flexible[small.front()] = true;
      small.clear();
    }
    std::vector<double> lengths(count, 0.0);
    std::vector<std::size_t> pinnedSmall;
    std::vector<std::size_t> merged;
    double mergedRaw = 0;
    for (const auto i : small) {
      if (pinned(items[i])) {
        pinnedSmall.push_back(i);
      } else {
        merged.push_back(i);
        mergedRaw += raw[i];
      }
    }
    const double mergedLength = merged.empty() ? 0.0 : std::max(minLength, mergedRaw);
    double fixed = mergedLength;
    for (const auto i : pinnedSmall) {
      lengths[i] = floors[i];
      fixed += lengths[i];
    }
    fillLengths(lengths, flexible, raw, floors, length, fixed);

    std::vector<BandSegment> order;
    double position = 0;
    const auto push = [&order, &position](BandSegment segment, double segmentLength) {
      segment.s0 = position;
      position += segmentLength;
      segment.s1 = position;
      order.push_back(std::move(segment));
    };
    const auto single = [](NodeId key) {
      BandSegment segment;
      segment.key = key;
      return segment;
    };
    for (std::size_t i = 0; i < count; ++i) {
      if (lengths[i] > 0 && !std::ranges::contains(pinnedSmall, i)) {
        push(single(items[i].key), lengths[i]);
      }
    }
    for (const auto i : pinnedSmall) {
      push(single(items[i].key), lengths[i]);
    }
    if (!merged.empty()) {
      BandSegment tail;
      tail.merged = true;
      tail.keys.reserve(merged.size());
      for (const auto i : merged) {
        tail.keys.push_back(items[i].key);
      }
      push(std::move(tail), mergedLength);
    }
    if (order.empty() || !(position > 0) || !std::isfinite(position)) {
      return {};
    }
    if (std::abs(position - length) > 1e-6) {
      const double factor = length / position;
      double at = 0;
      for (auto& segment : order) {
        const double scaled = (segment.s1 - segment.s0) * factor;
        segment.s0 = at;
        at += scaled;
        segment.s1 = at;
      }
    }
    order.back().s1 = length;
    return order;
  }

  std::vector<BandSegment> snapToStraights(const Band& band, std::vector<BandSegment> segments, double minLength) {
    for (std::size_t i = 0; i + 1 < segments.size(); ++i) {
      const double boundary = segments[i].s1;
      const std::size_t piece = bandPiece(band, boundary);
      if (piece % 2 == 0) {
        continue;
      }
      const double arcStart = band.starts[piece];
      const double arcEnd = arcStart + band.lengths[piece];
      const bool nearStart = boundary - arcStart < arcEnd - boundary;
      for (const double to : {nearStart ? arcStart : arcEnd, nearStart ? arcEnd : arcStart}) {
        if (to - segments[i].s0 >= minLength && segments[i + 1].s1 - to >= minLength) {
          segments[i].s1 = to;
          segments[i + 1].s0 = to;
          break;
        }
      }
    }
    return segments;
  }
} // namespace luciole::workbench
