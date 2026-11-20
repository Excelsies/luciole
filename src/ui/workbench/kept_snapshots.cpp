#include "ui/workbench/kept_snapshots.h"

#include "workbench/scene_shapes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_set>

namespace luciole {
  namespace {
    namespace wb = workbench;

    bool names(const PartSnapshot& shown, const PlacedSnapshot& kept) {
      return shown.tree != nullptr && kept.snapshot->serial == shown.serial && &kept.snapshot->tree == shown.tree.get();
    }

    constexpr double kMostFolded = 1e9; // a remainder's count past this is not believed

    // How many entries a remainder node folds; a count that is not finite or negative folds none.
    std::size_t foldedCount(const wb::Node& node) {
      return std::isfinite(node.items) && node.items > 0 ? static_cast<std::size_t>(std::min(node.items, kMostFolded))
                                                         : 0;
    }

    struct Weighed {
      wb::NodeId id = wb::kNoNode;
      double weight = 0;
    };

    // The order of workbench::entries: heavier first, then by name, then by name bytes, then by id.
    bool heavier(const wb::Tree& tree, const Weighed& left, const Weighed& right) {
      if (left.weight != right.weight) {
        return left.weight > right.weight;
      }
      const auto& a = tree.nodes[left.id].name;
      const auto& b = tree.nodes[right.id].name;
      if (const auto order = wb::compareNames(a, b); order != 0) {
        return order < 0;
      }
      return a != b ? a < b : left.id < right.id;
    }

    struct Box {
      float left = std::numeric_limits<float>::infinity();
      float top = std::numeric_limits<float>::infinity();
      float right = -std::numeric_limits<float>::infinity();
      float bottom = -std::numeric_limits<float>::infinity();

      void add(float x, float y) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
      }
    };

    // The sector's ends on both edges, and the outer edge where it passes a quarter turn: at most five of them.
    void addSector(Box& box, const wb::ShapeSector& sector) {
      constexpr float kQuarter = 90;
      constexpr int kMostQuarters = 5;
      const float end = sector.a0 + std::min(sector.a1 - sector.a0, 4 * kQuarter);
      const auto at = [&box, &sector](float radius, float degrees) {
        const float radians = degrees * std::numbers::pi_v<float> / 180;
        box.add(sector.center.x + (radius * std::sin(radians)), sector.center.y - (radius * std::cos(radians)));
      };
      for (const float radius : {sector.r0, sector.r1}) {
        at(radius, sector.a0);
        at(radius, end);
      }
      const float first = std::ceil(sector.a0 / kQuarter) * kQuarter;
      for (int quarter = 0; quarter < kMostQuarters && first + (kQuarter * static_cast<float>(quarter)) <= end;
           ++quarter) {
        at(sector.r1, first + (kQuarter * static_cast<float>(quarter)));
      }
    }

    void addShape(Box& box, const wb::Shape& shape) {
      switch (shape.kind) {
      case wb::ShapeKind::Rect:
      case wb::ShapeKind::RoundedRect:
        box.add(shape.rect.x, shape.rect.y);
        box.add(shape.rect.x + shape.rect.width, shape.rect.y + shape.rect.height);
        return;
      case wb::ShapeKind::Sector:
        addSector(box, shape.sector);
        return;
      case wb::ShapeKind::Circle:
        box.add(shape.circle.center.x - shape.circle.radius, shape.circle.center.y - shape.circle.radius);
        box.add(shape.circle.center.x + shape.circle.radius, shape.circle.center.y + shape.circle.radius);
        return;
      case wb::ShapeKind::Ribbon: // the control points hold the curves
        for (const auto& point : shape.ribbon.top) {
          box.add(point.x, point.y);
        }
        for (const auto& point : shape.ribbon.bottom) {
          box.add(point.x, point.y);
        }
        return;
      case wb::ShapeKind::Line:
        box.add(shape.line.from.x, shape.line.from.y);
        box.add(shape.line.to.x, shape.line.to.y);
        return;
      }
    }
  } // namespace

  std::optional<wb::ShapePoint> mergedAnchor(std::span<const wb::Shape> drawn, wb::NodeId folder) {
    Box box;
    for (const auto& shape : drawn) {
      if (shape.id.node == folder
          && shape.hittable
          && wb::sceneRole(shape.id.role) == wb::SceneRole::More
          && wb::shapeValid(shape)) {
        addShape(box, shape);
      }
    }
    if (box.left > box.right || box.top > box.bottom) { // nothing added
      return std::nullopt;
    }
    return wb::ShapePoint{.x = (box.left + box.right) / 2, .y = (box.top + box.bottom) / 2};
  }

  void KeptSnapshots::add(const PlacedSnapshot& placed) {
    if (!placed.snapshot) {
      return;
    }
    m_kept.push_back(placed);
    if (m_kept.size() > kMax) {
      m_kept.erase(m_kept.begin());
    }
  }

  void KeptSnapshots::keepFrom(std::span<const PartSnapshot> inUse) {
    const auto first = std::ranges::find_if(m_kept, [inUse](const PlacedSnapshot& kept) {
      return std::ranges::any_of(inUse, [&kept](const PartSnapshot& shown) { return names(shown, kept); });
    });
    m_kept.erase(m_kept.begin(), first);
  }

  const PlacedSnapshot* KeptSnapshots::find(const PartSnapshot& shown) const {
    const auto found =
        std::ranges::find_if(m_kept, [&shown](const PlacedSnapshot& kept) { return names(shown, kept); });
    return found != m_kept.end() ? &*found : nullptr;
  }

  std::optional<WorkbenchEntry> KeptSnapshots::entryIn(const PartSnapshot& shown, workbench::NodeId node) const {
    const auto* placed = find(shown);
    return placed != nullptr ? entryOf(*placed, node) : std::nullopt;
  }

  workbench::NodeId KeptSnapshots::nodeIn(const PartSnapshot& shown, const Location& location) const {
    const auto* placed = find(shown);
    return placed != nullptr ? nodeOf(*placed, location) : workbench::kNoNode;
  }

  MergedEntries KeptSnapshots::mergedIn(
      const PartSnapshot& shown, wb::NodeId folder, std::span<const wb::Shape> drawn,
      const wb::LayoutSettings& settings, bool foldersOnly, std::size_t limit
  ) const {
    const auto* placed = find(shown);
    if (placed == nullptr || !wb::hasNode(*shown.tree, folder) || !shown.tree->nodes[folder].folder) {
      return {};
    }
    const auto& tree = *shown.tree;
    std::unordered_set<wb::NodeId> own;
    for (const auto& shape : drawn) {
      own.insert(shape.id.node);
    }
    std::vector<Weighed> held;
    std::size_t folded = 0; // entries of the remainders, which no list can name
    for (const auto id : wb::children(tree, folder)) {
      const auto& node = tree.nodes[id];
      if (own.contains(id) || !wb::visibleEntry(node, settings.showHidden) || (foldersOnly && !node.folder)) {
        continue;
      }
      if (node.name.empty()) {
        folded += foldedCount(node);
      } else {
        held.push_back({.id = id, .weight = wb::weight(tree, id, settings)});
      }
    }
    const auto first = held.begin() + static_cast<std::ptrdiff_t>(std::min(limit, held.size()));
    std::partial_sort(held.begin(), first, held.end(), [&tree](const Weighed& a, const Weighed& b) {
      return heavier(tree, a, b);
    });
    MergedEntries merged{.listed = {}, .count = held.size() + folded};
    for (auto at = held.begin(); at != first; ++at) {
      if (auto entry = entryOf(*placed, at->id)) {
        merged.listed.push_back({
            .entry = std::move(*entry),
            .name = tree.nodes[at->id].name,
            .value = wb::measureLabel(tree, at->id, settings.sizeBy),
        });
      }
    }
    return merged;
  }
} // namespace luciole
