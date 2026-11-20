#include "workbench/flame_row.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr double kWantCap = 150;
    constexpr std::size_t kNamed = 12; // label widths are guaranteed only to the heaviest entries
    constexpr double kSlack = 1e-9;

    double cleanShare(double share) { return std::isfinite(share) && share > 0 ? share : 0.0; }

    double wantAt(std::span<const double> want, std::size_t index) {
      return index < want.size() && !std::isnan(want[index]) ? want[index] : 0.0;
    }

    // `total` split by `shares` with part i at least floors[i]: parts that would fall short are fixed at their floor
    // and the others share what is left by weight. Each round fixes at least one part, so at most n rounds.
    std::vector<double> waterFill(const std::vector<double>& shares, double total, const std::vector<double>& floors) {
      std::vector<double> widths(shares.size(), 0.0);
      std::vector<std::size_t> open(shares.size());
      std::ranges::iota(open, std::size_t{0});
      std::vector<bool> low(shares.size(), false);
      double remaining = total;
      while (!open.empty()) {
        double weight = 0;
        for (const auto i : open) {
          weight += shares[i];
        }
        const auto part = [&](std::size_t i) {
          return weight > 0 ? shares[i] / weight * remaining : remaining / static_cast<double>(open.size());
        };
        bool anyLow = false;
        for (const auto i : open) {
          low[i] = part(i) < floors[i];
          anyLow = anyLow || low[i];
        }
        if (!anyLow) {
          for (const auto i : open) {
            widths[i] = part(i);
          }
          return widths;
        }
        for (const auto i : open) {
          if (low[i]) {
            widths[i] = floors[i];
            remaining -= floors[i];
          }
        }
        std::erase_if(open, [&low](std::size_t i) { return low[i]; });
      }
      if (!widths.empty()) {
        widths[0] += remaining;
      }
      return widths;
    }

    // Widths of the shown entries over `total` px. With label widths, the heaviest are first guaranteed theirs (at
    // most 150 px) as far as the row allows, then all share the rest by weight above those floors.
    std::vector<double>
    fitWidths(const std::vector<double>& shares, double total, double min, const std::vector<double>* want) {
      if (want == nullptr) {
        double sum = 0;
        for (const double share : shares) {
          sum += share;
        }
        std::vector<double> widths;
        widths.reserve(shares.size());
        for (const double share : shares) {
          widths.push_back(sum > 0 ? share / sum * total : total / static_cast<double>(shares.size()));
        }
        return widths;
      }
      std::vector<double> floors;
      floors.reserve(shares.size());
      double need = 0;
      for (const double wanted : *want) {
        floors.push_back(std::max(min, std::min(wanted, kWantCap)));
        need += floors.back();
      }
      for (std::size_t guaranteed = floors.size(); guaranteed > 0 && need > total + kSlack;) {
        --guaranteed;
        need += min - floors[guaranteed];
        floors[guaranteed] = min;
      }
      return waterFill(shares, total, floors);
    }

    struct RestSplit {
      std::vector<std::size_t> shown;
      std::vector<double> widths;
      std::vector<std::size_t> merged;
      double mergedW = 0;
      bool hasMerged = false;
    };

    // How many of the unpinned entries show, how wide each is, and the merged piece for the others.
    RestSplit splitRest(
        std::vector<double>& shares, const std::vector<std::size_t>& rest, double room, double span, double min,
        double cap, std::span<const double> want
    ) {
      std::vector<double> prefix{0.0};
      for (const auto i : rest) {
        prefix.push_back(prefix.back() + shares[i]);
      }
      if (!(prefix.back() > 0)) {
        for (std::size_t j = 0; j < rest.size(); ++j) {
          shares[rest[j]] = 1;
          prefix[j + 1] = static_cast<double>(j + 1);
        }
      }
      const double total = prefix.back();
      const auto sliverAt = [&](std::size_t count) {
        return count < rest.size()
            ? std::min(std::max((total - prefix[count]) / total * room, min), std::max(min, cap * span))
            : 0.0;
      };
      std::size_t k = 0;
      while (k < rest.size() && shares[rest[k]] / total * room >= min) {
        ++k;
      }
      k = std::max<std::size_t>(1, k);
      if (!want.empty()) {
        const auto wanted = [&](std::size_t j) { return std::max(min, std::min(wantAt(want, rest[j]), kWantCap)); };
        double need = 0;
        for (std::size_t j = 0; j < k; ++j) {
          need += wanted(j);
        }
        while (k < std::min(rest.size(), kNamed) && need + wanted(k) <= room - sliverAt(k + 1)) {
          need += wanted(k);
          ++k;
        }
        while (k > 1 && room - sliverAt(k) < static_cast<double>(k) * min - kSlack) {
          --k;
        }
      } else {
        while (k > 1 && shares[rest[k - 1]] / prefix[k] * (room - sliverAt(k)) < min - kSlack) {
          --k;
        }
      }
      if (k < rest.size() && room < 2 * min) {
        k = 0;
      }
      RestSplit split;
      split.mergedW = k < rest.size() ? sliverAt(k) : 0.0;
      split.shown.assign(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(k));
      if (split.mergedW > 0) {
        split.hasMerged = true;
        split.merged.assign(rest.begin() + static_cast<std::ptrdiff_t>(k), rest.end());
      }
      if (split.shown.empty()) {
        return split;
      }
      std::vector<double> shownShares;
      std::vector<double> shownWant;
      for (const auto i : split.shown) {
        shownShares.push_back(shares[i]);
        shownWant.push_back(wantAt(want, i));
      }
      split.widths = fitWidths(shownShares, room - split.mergedW, min, want.empty() ? nullptr : &shownWant);
      return split;
    }
  } // namespace

  double labelTextWidth(std::string_view text, const LabelFont& font) {
    double width = 0;
    for (const char c : text) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte < font.ascii.size()) {
        width += font.ascii[byte];
      } else if ((byte & 0xC0U) != 0x80U) {
        width += font.other;
      }
    }
    return width;
  }

  FlameRowSplit flameRowLayout(std::span<const double> shares, double x0, double x1, const FlameRowOptions& options) {
    FlameRowSplit result;
    const std::size_t count = shares.size();
    const double span = x1 - x0;
    if (count == 0 || !(span > 0) || !std::isfinite(span)) {
      return result;
    }
    const double min = std::isfinite(options.min) && options.min > 0 ? options.min : 0.0;
    const double cap = std::isfinite(options.cap) ? options.cap : kFlameRowCap;
    const std::size_t pin = options.pin < count ? options.pin : kNoFlameItem;
    std::vector<double> clean(count);
    std::ranges::transform(shares, clean.begin(), cleanShare);
    const double pinMin = std::isnan(options.pinMin) ? 0.0 : options.pinMin;
    double pinW = pin != kNoFlameItem ? std::min(span, std::max(clean[pin] * span, pinMin)) : 0.0;
    std::vector<std::size_t> rest;
    rest.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      if (i != pin) {
        rest.push_back(i);
      }
    }
    if (!rest.empty() && span - pinW < min) {
      pinW = span; // no room for anything else: the pinned entry takes it all
    }
    const double room = span - pinW;
    std::vector<std::pair<std::size_t, double>> order;
    RestSplit split;
    if (!rest.empty() && room >= min) {
      split = splitRest(clean, rest, room, span, min, cap, options.want);
      for (std::size_t j = 0; j < split.shown.size(); ++j) {
        order.emplace_back(split.shown[j], split.widths[j]);
      }
      if (pin != kNoFlameItem) {
        order.emplace_back(pin, pinW);
      }
      std::ranges::sort(order, std::less{}, &std::pair<std::size_t, double>::first);
    } else if (pin != kNoFlameItem) {
      order.emplace_back(pin, pinW);
    }
    if (order.empty() && !split.hasMerged && !rest.empty() && room >= min) {
      split.hasMerged = true;
      split.merged = rest;
    }
    double x = x0;
    for (const auto& [index, w] : order) {
      result.segs.push_back({.index = index, .x = x, .w = w});
      x += w;
    }
    if (split.hasMerged) {
      result.more = FlameMore{.x = x, .w = x1 - x, .indices = std::move(split.merged)};
    } else if (!result.segs.empty()) {
      result.segs.back().w = x1 - result.segs.back().x; // the row ends exactly at x1
    }
    return result;
  }
} // namespace luciole::workbench
