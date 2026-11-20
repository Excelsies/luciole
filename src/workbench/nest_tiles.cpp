#include "workbench/nest_tiles.h"

#include "workbench/nest_labels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr double kMoreMax = 0.10;
    constexpr double kTileEdge = 2;
    constexpr double kShareSlack = 1e-9;
    constexpr std::size_t kProbeAhead = 12;
    // Squarified rows are not exactly square: each pass asks for this many minimum areas per tile, the least first.
    constexpr std::array<double, 9> kAreaMargins{1.0, 1.15, 1.3, 1.5, 1.8, 2.2, 3.0, 4.5, 6.0};

    // A tile's label pads its name by 8 px on each side and the tile is inset by 1 px on each.
    constexpr double kNameChrome = 18;

    bool usable(double weight) { return std::isfinite(weight) && weight > 0; }

    double namesOf(std::span<const double> names, std::size_t i) { return i < names.size() ? names[i] : 0.0; }

    bool allMeet(const std::vector<TreemapRect>& rects, NestSize min, std::span<const double> names) {
      if (rects.empty()) {
        return false;
      }
      for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        const double name = namesOf(names, i);
        if (r.w < min.w || r.h < min.h || (name > 0 && nestTileNameRoom(inset(r, kTileEdge / 2)) < name)) {
          return false;
        }
      }
      return true;
    }

    std::vector<TreemapRect> marginTiles(
        std::span<const double> weights, TreemapRect rect, NestSize min, double margin, std::span<const double> names
    ) {
      std::vector<double> floors(weights.size());
      for (std::size_t i = 0; i < floors.size(); ++i) {
        const double width = std::max(min.w, namesOf(names, i) > 0 ? namesOf(names, i) + kNameChrome : 0.0);
        floors[i] = width * min.h * margin / (rect.w * rect.h);
      }
      const auto shares = floorShares(weights, floors);
      if (shares.empty()) {
        return {};
      }
      // Cells that are square in a rectangle squeezed to the minimum's proportions are as wide as the minimum.
      const double squeeze = min.h / min.w;
      auto rects = squarify(shares, {.x = rect.x * squeeze, .y = rect.y, .w = rect.w * squeeze, .h = rect.h});
      for (auto& r : rects) {
        r.x /= squeeze;
        r.w /= squeeze;
      }
      return rects;
    }
  } // namespace

  std::vector<double> floorShares(std::span<const double> weights, std::span<const double> floors) {
    const std::size_t count = weights.size();
    if (count == 0 || !std::ranges::all_of(weights, usable)) {
      return {};
    }
    std::vector<double> floor(count, 0.0);
    double floorSum = 0;
    for (std::size_t i = 0; i < count; ++i) {
      floor[i] = i < floors.size() ? floors[i] : 0.0;
      if (!std::isfinite(floor[i]) || floor[i] < 0) {
        return {};
      }
      floorSum += floor[i];
    }
    if (floorSum > 1.0 + kShareSlack) {
      return {};
    }
    std::vector<bool> pinned(count, false);
    double pinnedSum = 0;
    std::size_t pins = 0;
    double scale = 0;
    for (std::size_t pass = 0; pass <= count; ++pass) {
      double free = 0;
      for (std::size_t i = 0; i < count; ++i) {
        free += pinned[i] ? 0.0 : weights[i];
      }
      scale = (1.0 - pinnedSum) / free;
      bool changed = false;
      for (std::size_t i = 0; i < count; ++i) {
        if (!pinned[i] && weights[i] * scale < floor[i]) {
          pinned[i] = true;
          pinnedSum += floor[i];
          ++pins;
          changed = true;
        }
      }
      if (!changed || pins == count) {
        break;
      }
    }
    std::vector<double> shares(count);
    for (std::size_t i = 0; i < count; ++i) {
      shares[i] = pinned[i] ? floor[i] : weights[i] * scale;
    }
    return shares;
  }

  std::vector<TreemapRect>
  minimumTiles(std::span<const double> weights, TreemapRect rect, NestSize min, std::span<const double> names) {
    const bool finite =
        std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.w) && std::isfinite(rect.h);
    if (weights.empty() || !finite || !(rect.w > 0) || !(rect.h > 0) || !(min.w > 0) || !(min.h > 0)) {
      return {};
    }
    // Pure proportions first: when they already give every tile the minimum nothing needs to change.
    if (auto plain = squarify(weights, rect); allMeet(plain, min, names)) {
      return plain;
    }
    for (const double margin : kAreaMargins) {
      auto rects = marginTiles(weights, rect, min, margin, names);
      if (allMeet(rects, min, names)) {
        return rects;
      }
    }
    return {};
  }

  NestTileLayout layoutNestTiles(
      std::span<const double> weights, TreemapRect rect, NestSize min, std::size_t maxShown,
      std::span<const double> names
  ) {
    const std::size_t count = weights.size();
    if (count == 0 || !std::isfinite(rect.w) || !std::isfinite(rect.h) || rect.w < min.w || rect.h < min.h) {
      return {};
    }
    std::vector<double> prefix(count + 1, 0.0);
    for (std::size_t i = 0; i < count; ++i) {
      prefix[i + 1] = prefix[i] + weights[i];
    }
    const double total = prefix[count];
    const double moreFloor = std::min(0.4, (min.w * min.h * 1.4) / (rect.w * rect.h));
    const auto chosenFor = [&](std::size_t shown) {
      std::vector<double> chosen(weights.begin(), weights.begin() + static_cast<std::ptrdiff_t>(shown));
      if (shown < count) {
        const double share = std::min(kMoreMax, std::max(moreFloor, (total - prefix[shown]) / total));
        chosen.push_back((share / (1 - share)) * prefix[shown]);
      }
      return chosen;
    };
    const auto rectsFor = [&](std::size_t shown) {
      return minimumTiles(chosenFor(shown), rect, min, names.first(std::min(shown, names.size())));
    };
    // Entries and the "+N more" tile each need the minimum's area.
    const double room = std::floor(rect.w * rect.h / (min.w * min.h));
    const std::size_t roomFor = room >= static_cast<double>(count) ? count : static_cast<std::size_t>(room);
    std::size_t low = 1;
    std::size_t high = std::max<std::size_t>(1, std::min({count, maxShown, roomFor}));
    // Halves the range each step.
    while (low < high) {
      const std::size_t mid = (low + high + 1) / 2;
      if (!rectsFor(mid).empty()) {
        low = mid;
      } else {
        high = mid - 1;
      }
    }
    // Squarified rows meet the minimum at some counts and not at the next, so look a few counts further.
    const std::size_t last = std::min({count, maxShown, roomFor, low + kProbeAhead});
    for (std::size_t probe = last; probe > low; --probe) {
      if (!rectsFor(probe).empty()) {
        low = probe;
        break;
      }
    }
    const std::size_t shown = low;
    const auto rects = rectsFor(shown);
    if (rects.empty()) {
      if (count == 1) {
        return {}; // a weight that cannot be laid out
      }
      const double strip = min.h + 4;
      if (rect.h < strip + min.h) {
        return {.tiles = {}, .more = inset(rect, kTileEdge / 2)};
      }
      const TreemapRect top{.x = rect.x, .y = rect.y, .w = rect.w, .h = rect.h - strip};
      const TreemapRect bottom{.x = rect.x, .y = rect.y + top.h, .w = rect.w, .h = strip};
      return {.tiles = {inset(top, kTileEdge / 2)}, .more = inset(bottom, kTileEdge / 2)};
    }
    NestTileLayout layout;
    for (std::size_t i = 0; i < shown; ++i) {
      layout.tiles.push_back(inset(rects[i], kTileEdge / 2));
    }
    if (shown < count) {
      layout.more = inset(rects[shown], kTileEdge / 2);
    }
    return layout;
  }
} // namespace luciole::workbench
