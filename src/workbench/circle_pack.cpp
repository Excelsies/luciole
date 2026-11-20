#include "workbench/circle_pack.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace luciole::workbench {
  namespace {
    constexpr double kEpsilon = 1e-9;
    constexpr double kOverlapSlack = 1e-7;
    constexpr int kSearchSteps = 40;

    struct Spot {
      double x = 0;
      double y = 0;
    };

    // True when a circle of radius r at (x, y) overlaps none of the placed ones.
    bool fits(std::span<const Circle> placed, double x, double y, double r) {
      return std::ranges::none_of(placed, [&](const Circle& other) {
        const double dx = other.x - x;
        const double dy = other.y - y;
        const double least = other.r + r - kOverlapSlack;
        return dx * dx + dy * dy < least * least;
      });
    }

    // Free spot for a circle of radius r touching two placed circles, closest to the origin; ties go to the
    // smaller y, then the smaller x.
    std::optional<Spot> bestSpot(std::span<const Circle> placed, double r) {
      Spot best;
      double bestDistance = std::numeric_limits<double>::infinity();
      bool found = false;
      for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
          const double dx = placed[j].x - placed[i].x;
          const double dy = placed[j].y - placed[i].y;
          const double d2 = dx * dx + dy * dy;
          const double d = std::sqrt(d2);
          const double a = placed[i].r + r;
          const double b = placed[j].r + r;
          if (d < kEpsilon || d > a + b + kEpsilon || d < std::abs(a - b)) {
            continue;
          }
          const double along = (a * a - b * b + d2) / (2 * d);
          const double h = std::sqrt(std::max(0.0, a * a - along * along));
          const double mx = placed[i].x + (along * dx) / d;
          const double my = placed[i].y + (along * dy) / d;
          const double ox = (-h * dy) / d;
          const double oy = (h * dx) / d;
          for (const double side : {-1.0, 1.0}) {
            const double px = mx + side * ox;
            const double py = my + side * oy;
            const double distance = px * px + py * py;
            if (distance > bestDistance
                || (distance == bestDistance && found && (py > best.y || (py == best.y && px >= best.x)))) {
              continue;
            }
            if (fits(placed, px, py, r)) {
              best = {.x = px, .y = py};
              bestDistance = distance;
              found = true;
            }
          }
        }
      }
      if (!found) {
        return std::nullopt;
      }
      return best;
    }

    // Largest distance from (cx, cy) to the far edge of any circle.
    double reach(std::span<const Circle> circles, double cx, double cy) {
      double most = 0;
      for (const auto& circle : circles) {
        const double dx = circle.x - cx;
        const double dy = circle.y - cy;
        most = std::max(most, std::sqrt(dx * dx + dy * dy) + circle.r);
      }
      return most;
    }

    // Ternary search for the minimum of a convex function on [lo, hi].
    template <typename Cost> double ternaryMinimum(double lo, double hi, Cost cost) {
      for (int step = 0; step < kSearchSteps; ++step) {
        const double a = lo + (hi - lo) / 3;
        const double b = hi - (hi - lo) / 3;
        if (cost(a) < cost(b)) {
          hi = b;
        } else {
          lo = a;
        }
      }
      return (lo + hi) / 2;
    }

    // Smallest circle enclosing all circles, by nested ternary searches over the center.
    Circle enclose(std::span<const Circle> circles) {
      double x0 = std::numeric_limits<double>::infinity();
      double x1 = -x0;
      double y0 = x0;
      double y1 = -x0;
      for (const auto& circle : circles) {
        x0 = std::min(x0, circle.x);
        x1 = std::max(x1, circle.x);
        y0 = std::min(y0, circle.y);
        y1 = std::max(y1, circle.y);
      }
      const auto bestY = [&](double cx) {
        return ternaryMinimum(y0, y1, [&](double cy) { return reach(circles, cx, cy); });
      };
      const double cx = ternaryMinimum(x0, x1, [&](double x) { return reach(circles, x, bestY(x)); });
      const double cy = bestY(cx);
      return {.x = cx, .y = cy, .r = reach(circles, cx, cy)};
    }

    // Two NaN emphases weigh entries the same way, so they count as one setting.
    bool sameEmphasis(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }
  } // namespace

  std::expected<std::vector<Circle>, PackError> packCircles(std::span<const double> radii) {
    if (std::ranges::any_of(radii, [](double r) { return !std::isfinite(r) || !(r > 0); })) {
      return std::unexpected(PackError::BadRadius);
    }
    std::vector<Circle> circles;
    circles.reserve(radii.size());
    for (const double r : radii) {
      circles.push_back({.r = r});
    }
    if (circles.size() > 1) {
      circles[1].x = circles[0].r + circles[1].r;
    }
    for (std::size_t k = 2; k < circles.size(); ++k) {
      const auto spot = bestSpot(std::span(circles).first(k), circles[k].r);
      if (!spot) {
        return std::unexpected(PackError::NoSpot);
      }
      circles[k].x = spot->x;
      circles[k].y = spot->y;
    }
    if (circles.empty()) {
      return circles;
    }
    const auto outer = enclose(circles);
    for (auto& circle : circles) {
      circle = {.x = (circle.x - outer.x) / outer.r, .y = (circle.y - outer.y) / outer.r, .r = circle.r / outer.r};
    }
    return circles;
  }

  std::optional<PackedItem> FolderPacking::placement(NodeId child) const {
    const auto found = std::ranges::find(items, child, &PackedItem::node);
    if (child == kNoNode || found == items.end()) {
      return std::nullopt;
    }
    return *found;
  }

  double moreWeight(std::span<const double> shownWeights, double restWeight) {
    if (shownWeights.empty()) {
      return restWeight;
    }
    std::vector<double> sorted(shownWeights.begin(), shownWeights.end());
    std::ranges::sort(sorted);
    return std::max(std::min(restWeight, sorted[sorted.size() / 2]), sorted.front());
  }

  FolderPacking
  packFolder(const Tree& tree, NodeId folder, const LayoutSettings& settings, std::span<const NodeId> extra) {
    constexpr double kContentRadius = 0.9;
    constexpr double kContentOffset = 0.06;
    struct Entry {
      NodeId node = kNoNode;
      double weight = 0;
    };
    if (!hasNode(tree, folder)) {
      return {};
    }
    const auto list = entries(tree, folder, settings);
    const auto shownCount = std::min(list.ids.size(), kAtlasMaxShown);
    std::vector<Entry> shown;
    shown.reserve(shownCount + extra.size() + 1);
    for (std::size_t i = 0; i < shownCount; ++i) {
      shown.push_back({.node = list.ids[i], .weight = list.weights[i]});
    }
    std::vector<NodeId> added;
    for (const auto id : extra) {
      const bool listed =
          std::ranges::contains(list.ids.begin(), list.ids.begin() + static_cast<std::ptrdiff_t>(shownCount), id);
      const bool child = hasNode(tree, id) && tree.nodes[id].parent == folder;
      if (child && !listed && !std::ranges::contains(added, id)) {
        shown.push_back({.node = id, .weight = weight(tree, id, settings)});
        added.push_back(id);
      }
    }
    FolderPacking packing;
    double restWeight = 0;
    for (std::size_t i = shownCount; i < list.ids.size(); ++i) {
      if (!std::ranges::contains(added, list.ids[i])) {
        packing.rest.push_back(list.ids[i]);
        restWeight += list.weights[i];
      }
    }
    if (!packing.rest.empty()) {
      std::vector<double> weights;
      weights.reserve(shown.size());
      for (const auto& entry : shown) {
        weights.push_back(entry.weight);
      }
      shown.push_back({.node = kNoNode, .weight = moreWeight(weights, restWeight)});
    }
    std::ranges::stable_sort(shown, std::greater{}, &Entry::weight);
    std::vector<double> radii;
    radii.reserve(shown.size());
    for (const auto& entry : shown) {
      radii.push_back(std::sqrt(entry.weight));
    }
    const auto circles = packCircles(radii);
    if (!circles) {
      packing.failed = true;
      return packing;
    }
    packing.items.reserve(shown.size());
    for (std::size_t i = 0; i < shown.size(); ++i) {
      const auto& circle = (*circles)[i];
      packing.items.push_back(
          {.node = shown[i].node,
           .more = shown[i].node == kNoNode,
           .x = circle.x * kContentRadius,
           .y = circle.y * kContentRadius + kContentOffset,
           .r = circle.r * kContentRadius}
      );
    }
    return packing;
  }

  std::shared_ptr<const FolderPacking> PackingCache::packing(
      const Tree& tree, NodeId folder, const LayoutSettings& settings, std::span<const NodeId> force
  ) {
    if (!hasNode(tree, folder)) {
      return std::make_shared<const FolderPacking>();
    }
    const bool sameSettings = m_settings
        && m_settings->sizeBy == settings.sizeBy
        && sameEmphasis(m_settings->emphasis, settings.emphasis)
        && m_settings->showHidden == settings.showHidden;
    if (!sameSettings) {
      clear();
      m_settings = settings;
    }
    Key plainKey{folder, {}};
    const auto plainFound = m_packings.find(plainKey);
    auto plain = plainFound != m_packings.end() ? plainFound->second
                                                : remember(plainKey, packFolder(tree, folder, settings, {}));
    std::vector<NodeId> missing;
    for (const auto id : force) {
      if (hasNode(tree, id)
          && tree.nodes[id].parent == folder
          && !plain->placement(id)
          && !std::ranges::contains(missing, id)) {
        missing.push_back(id);
      }
    }
    if (missing.empty()) {
      return plain;
    }
    Key forcedKey{folder, missing};
    std::ranges::sort(forcedKey.second);
    const auto forcedFound = m_packings.find(forcedKey);
    if (forcedFound != m_packings.end()) {
      return forcedFound->second;
    }
    return remember(std::move(forcedKey), packFolder(tree, folder, settings, missing));
  }

  std::shared_ptr<const FolderPacking> PackingCache::remember(Key key, FolderPacking packing) {
    auto shared = std::make_shared<const FolderPacking>(std::move(packing));
    m_packings.emplace(key, shared);
    m_order.push_back(std::move(key));
    if (m_order.size() > kAtlasMaxCached) {
      m_packings.erase(m_order.front());
      m_order.pop_front();
    }
    return shared;
  }

  void PackingCache::clear() {
    m_packings.clear();
    m_order.clear();
    m_settings.reset();
  }

  std::size_t PackingCache::size() const { return m_packings.size(); }
} // namespace luciole::workbench
