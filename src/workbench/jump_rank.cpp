#include "workbench/jump_rank.h"

#include "workbench/not_counted.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string_view>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr std::size_t kNoItem = std::numeric_limits<std::size_t>::max();

    // A node read by the walk; `from` is the walk item of its parent, kNoItem on level 1.
    struct WalkItem {
      NodeId id = kNoNode;
      std::size_t level = 0;
      std::size_t from = kNoItem;
    };

    struct Candidate {
      std::size_t item = 0;
      double visits = 0;
      double bytes = 0;
    };

    // Names from level 1 down to the item, joined by '/'. Bounded by the item's level.
    std::string walkPath(const Tree& tree, const std::vector<WalkItem>& walk, std::size_t item) {
      std::vector<const std::string*> names;
      for (auto at = item; at != kNoItem && names.size() <= walk[item].level; at = walk[at].from) {
        names.push_back(&tree.nodes[walk[at].id].name);
      }
      std::string path;
      for (const auto* name : std::views::reverse(names)) {
        if (!path.empty()) {
          path += '/';
        }
        path += *name;
      }
      return path;
    }

    // The byte that follows `name` in a walk path at `at`: its own byte, then '/' when the path goes deeper, else -1.
    int byteAfter(std::string_view name, std::size_t at, bool deeper) {
      if (at < name.size()) {
        return static_cast<unsigned char>(name[at]);
      }
      return deeper ? '/' : -1;
    }

    // Whether the walk path of `left` sorts before that of `right` in byte order, without building either one in
    // the usual case: the paths share everything down to their nearest common folder, so the two items below it
    // decide. Both climbs are bounded by the items' levels.
    bool pathBefore(const Tree& tree, const std::vector<WalkItem>& walk, std::size_t left, std::size_t right) {
      if (left == right) {
        return false;
      }
      auto upLeft = left;
      auto upRight = right;
      while (walk[upLeft].level > walk[upRight].level) {
        upLeft = walk[upLeft].from;
      }
      while (walk[upRight].level > walk[upLeft].level) {
        upRight = walk[upRight].from;
      }
      if (upLeft == upRight) {
        return walk[left].level < walk[right].level; // an ancestor's path is a prefix
      }
      while (walk[upLeft].from != walk[upRight].from) {
        upLeft = walk[upLeft].from;
        upRight = walk[upRight].from;
      }
      const std::string_view leftName = tree.nodes[walk[upLeft].id].name;
      const std::string_view rightName = tree.nodes[walk[upRight].id].name;
      const auto common = std::min(leftName.size(), rightName.size());
      if (const auto order = leftName.substr(0, common).compare(rightName.substr(0, common)); order != 0) {
        return order < 0;
      }
      const auto leftByte = byteAfter(leftName, common, upLeft != left);
      const auto rightByte = byteAfter(rightName, common, upRight != right);
      if (leftByte != rightByte && !leftName.empty() && !rightName.empty()) {
        return leftByte < rightByte;
      }
      // Equal names, an empty name or a '/' in one, which no folder listing gives: compare the whole paths.
      return walkPath(tree, walk, left) < walkPath(tree, walk, right);
    }
  } // namespace

  JumpList jumpAhead(const Tree& tree, const LayoutSettings& settings, const JumpOptions& options) {
    JumpList list;
    if (!hasNode(tree, tree.root)) {
      return list;
    }
    list.partial = tree.nodes[tree.root].partial;
    std::vector<bool> seen(tree.nodes.size(), false);
    seen[tree.root] = true;
    std::vector<WalkItem> walk;
    // Queues the unseen children of a node; the queue never grows past the node budget.
    const auto enqueue = [&](NodeId parent, std::size_t level, std::size_t from) {
      for (const auto child : children(tree, parent)) {
        if (!hasNode(tree, child) || seen[child]) {
          continue;
        }
        if (walk.size() >= options.nodeBudget) {
          list.cut = true;
          return;
        }
        seen[child] = true;
        walk.push_back({.id = child, .level = level, .from = from});
      }
    };
    enqueue(tree.root, 1, kNoItem);
    std::vector<Candidate> candidates;
    // Breadth first; each node is queued once and the queue is bounded by the budget.
    for (std::size_t item = 0; item < walk.size(); ++item) {
      const auto [id, level, from] = walk[item];
      const auto& node = tree.nodes[id];
      if (!node.folder || !visibleEntry(node, settings.showHidden)) {
        continue;
      }
      list.partial = list.partial || node.partial;
      if (level >= kJumpMinLevel && !node.name.empty()) {
        candidates.push_back({
            .item = item,
            .visits = measureValue(tree, id, WorkbenchMeasure::Visits),
            .bytes = measureValue(tree, id, WorkbenchMeasure::Bytes),
        });
      }
      enqueue(id, level + 1, item);
    }
    const auto before = [&](const Candidate& left, const Candidate& right) {
      if (left.visits != right.visits) {
        return left.visits > right.visits;
      }
      if (left.bytes != right.bytes) {
        return left.bytes > right.bytes;
      }
      return pathBefore(tree, walk, left.item, right.item);
    };
    const auto shown = std::min(options.count, candidates.size());
    std::ranges::partial_sort(candidates, candidates.begin() + static_cast<std::ptrdiff_t>(shown), before);
    list.rows.reserve(shown);
    for (std::size_t i = 0; i < shown; ++i) {
      const auto id = walk[candidates[i].item].id;
      auto path = walkPath(tree, walk, candidates[i].item);
      auto split = splitRelativePath(path, kJumpPathChars);
      list.rows.push_back({
          .id = id,
          .path = std::move(path),
          .split = std::move(split),
          .label = measureLabel(tree, id, settings.sizeBy),
          .share = share(tree, id, settings),
          .partial = tree.nodes[id].partial,
          .notCounted = notCountedText(tree, id),
      });
    }
    return list;
  }
} // namespace luciole::workbench
