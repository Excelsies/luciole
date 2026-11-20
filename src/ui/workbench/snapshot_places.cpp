#include "ui/workbench/snapshot_places.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    constexpr std::size_t kMaxLevelsBelow = 8;  // the deepest a measured tree reaches below its folder
    constexpr std::size_t kMaxPathDepth = 2048; // PATH_MAX allows no more components

    // The node of the tree standing for `at`: the current folder or one of the ancestors placed.
    wb::NodeId anchorNode(const PlacedSnapshot& placed, const Location& at) {
      const auto& snapshot = *placed.snapshot;
      if (at == snapshot.location) {
        return snapshot.tree.root;
      }
      const std::size_t count = std::min(placed.ancestors.size(), snapshot.tree.ancestors.size());
      for (std::size_t i = 0; i < count; ++i) {
        if (at == placed.ancestors[i]) {
          return snapshot.tree.ancestors[i];
        }
      }
      return wb::kNoNode;
    }

    // The location of `anchor` itself: the current folder or a placed ancestor; none for any other node.
    std::optional<Location> anchorLocation(const PlacedSnapshot& placed, wb::NodeId node) {
      const auto& snapshot = *placed.snapshot;
      if (node == snapshot.tree.root) {
        return snapshot.location;
      }
      const auto& ancestors = snapshot.tree.ancestors;
      const auto found = std::ranges::find(ancestors, node);
      if (found == ancestors.end()) {
        return std::nullopt;
      }
      const auto index = static_cast<std::size_t>(std::distance(ancestors.begin(), found));
      if (index < placed.ancestors.size()) {
        return placed.ancestors[index];
      }
      return std::nullopt;
    }

    bool isAnchor(const wb::Tree& tree, wb::NodeId node) {
      return node == tree.root || std::ranges::find(tree.ancestors, node) != tree.ancestors.end();
    }
  } // namespace

  PlacedSnapshot placeSnapshot(std::shared_ptr<const WorkbenchSnapshot> snapshot) {
    PlacedSnapshot placed{.snapshot = std::move(snapshot), .ancestors = {}};
    if (!placed.snapshot) {
      return placed;
    }
    const std::size_t count = placed.snapshot->tree.ancestors.size();
    placed.ancestors.reserve(count);
    auto at = placed.snapshot->location.parent();
    for (std::size_t i = 0; i < count && at; ++i) {
      placed.ancestors.push_back(*at);
      at = at->parent();
    }
    return placed;
  }

  std::optional<WorkbenchEntry> entryOf(const PlacedSnapshot& placed, wb::NodeId node) {
    if (!placed.snapshot || !wb::hasNode(placed.snapshot->tree, node)) {
      return std::nullopt;
    }
    const auto& tree = placed.snapshot->tree;
    std::vector<const std::string*> names; // nearest first
    auto at = node;
    for (std::size_t step = 0; step <= tree.nodes.size() && wb::hasNode(tree, at); ++step) {
      if (isAnchor(tree, at)) {
        auto location = anchorLocation(placed, at);
        if (!location) {
          return std::nullopt;
        }
        for (const auto* name : names | std::views::reverse) {
          location = location->child(*name);
        }
        return WorkbenchEntry{.location = std::move(*location), .folder = tree.nodes[node].folder};
      }
      const auto& current = tree.nodes[at];
      if (current.name.empty()) {
        return std::nullopt; // a folded remainder, or a node below one
      }
      names.push_back(&current.name);
      at = current.parent;
    }
    return std::nullopt;
  }

  wb::NodeId nodeOf(const PlacedSnapshot& placed, const Location& location) {
    if (!placed.snapshot) {
      return wb::kNoNode;
    }
    const auto& tree = placed.snapshot->tree;
    std::vector<std::string> names; // nearest first
    auto at = location;
    for (std::size_t step = 0; step <= kMaxLevelsBelow; ++step) {
      auto node = anchorNode(placed, at);
      if (node != wb::kNoNode) {
        for (const auto& name : names | std::views::reverse) {
          const auto children = wb::children(tree, node);
          const auto found = std::ranges::find_if(children, [&](wb::NodeId id) { return tree.nodes[id].name == name; });
          if (found == children.end()) {
            return wb::kNoNode;
          }
          node = *found;
        }
        return node;
      }
      auto name = at.basename();
      auto parent = at.parent();
      if (!name || !parent) {
        return wb::kNoNode;
      }
      names.push_back(std::move(*name));
      at = std::move(*parent);
    }
    return wb::kNoNode;
  }

  std::optional<Location> jumpLocation(const PlacedSnapshot& placed, wb::NodeId node) {
    if (!placed.snapshot) {
      return std::nullopt;
    }
    const auto& snapshot = *placed.snapshot;
    for (std::size_t i = 0; i < snapshot.jump.rows.size() && i < snapshot.jumpLocations.size(); ++i) {
      if (snapshot.jump.rows[i].id == node) {
        return snapshot.jumpLocations[i];
      }
    }
    return std::nullopt;
  }

  std::optional<Location> entryWithin(const Location& folder, const Location& location) {
    if (!location.isInside(folder)) {
      return std::nullopt;
    }
    auto at = location;
    for (std::size_t step = 0; step < kMaxPathDepth; ++step) {
      auto parent = at.parent();
      if (!parent) {
        return std::nullopt;
      }
      if (*parent == folder) {
        return at;
      }
      at = std::move(*parent);
    }
    return std::nullopt;
  }
} // namespace luciole
