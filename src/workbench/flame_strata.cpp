#include "workbench/flame_strata.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>

namespace luciole::workbench {
  namespace {
    constexpr std::array<double, 3> kMinSegment{28, 18, 18};
    constexpr double kFirstCap = 0.1;
    constexpr std::size_t kWantRows = 48; // label widths are measured only for the heaviest entries
    constexpr double kLabelPad = 16;      // room a label needs beyond its text

    struct FolderShares {
      std::vector<NodeId> dirs;
      std::vector<double> shares; // among the folders only, heaviest first
    };

    FolderShares folderShares(const Tree& tree, const LayoutSettings& settings, NodeId folder) {
      FolderShares result;
      const auto list = entries(tree, folder, settings);
      double total = 0;
      for (std::size_t i = 0; i < list.ids.size(); ++i) {
        if (!tree.nodes[list.ids[i]].folder) {
          continue;
        }
        result.dirs.push_back(list.ids[i]);
        result.shares.push_back(list.total > 0 ? list.weights[i] / list.total : 0.0);
        total += result.shares.back();
      }
      for (auto& share : result.shares) {
        share = total > 0 ? share / total : 1.0 / static_cast<double>(result.dirs.size());
      }
      return result;
    }

    std::vector<double> wantFor(const Tree& tree, const std::vector<NodeId>& dirs, const LabelFont& font) {
      std::vector<double> want;
      const std::size_t count = std::min(dirs.size(), kWantRows);
      want.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        want.push_back(labelTextWidth(tree.nodes[dirs[i]].name, font) + kLabelPad);
      }
      return want;
    }

    struct Parent {
      NodeId node = kNoNode;
      double x0 = 0;
      double x1 = 0;
      std::size_t item = kNoFlameItem; // index in the previous row
    };

    // Items of one parent's folders in a stratum ahead, appended to `row`; their spans go to `next`.
    void layParent(
        const Tree& tree, const LayoutSettings& settings, const FlameAheadOptions& options, std::size_t level,
        const Parent& parent, FlameAhead& result, std::vector<Parent>& next
    ) {
      const auto folders = folderShares(tree, settings, parent.node);
      if (folders.dirs.empty()) {
        return;
      }
      const auto want = options.fonts
          ? wantFor(tree, folders.dirs, level == 0 ? options.fonts->firstAhead : options.fonts->ahead)
          : std::vector<double>{};
      const FlameRowOptions rowOptions{
          .min = options.min ? *options.min : kMinSegment[level],
          .cap = level == 0 && options.cap == kFlameRowCap ? kFirstCap : options.cap,
          .want = want,
      };
      const auto split = flameRowLayout(folders.shares, parent.x0, parent.x1, rowOptions);
      auto& row = result.rows[level];
      const std::size_t top = level == 0 ? kNoFlameItem : result.rows[level - 1][parent.item].top;
      for (const auto& seg : split.segs) {
        FlameItem item;
        item.node = folders.dirs[seg.index];
        item.level = level;
        item.x = seg.x;
        item.w = seg.w;
        item.parent = parent.item;
        item.top = level == 0 ? row.size() : top;
        next.push_back({.node = item.node, .x0 = seg.x, .x1 = seg.x + seg.w, .item = row.size()});
        row.push_back(std::move(item));
      }
      if (split.more) {
        FlameItem more;
        more.more = true;
        more.owner = parent.node;
        for (const auto index : split.more->indices) {
          more.list.push_back(folders.dirs[index]);
        }
        more.level = level;
        more.x = split.more->x;
        more.w = split.more->w;
        more.parent = parent.item;
        more.top = top;
        row.push_back(std::move(more));
      }
    }

    // Sorts a row by x and moves the indexes that point into it.
    void sortRow(std::vector<FlameItem>& row, std::vector<Parent>& next, bool firstRow) {
      std::vector<std::size_t> order(row.size());
      std::ranges::iota(order, std::size_t{0});
      std::ranges::stable_sort(order, std::less{}, [&row](std::size_t i) { return row[i].x; });
      std::vector<std::size_t> position(row.size());
      for (std::size_t i = 0; i < order.size(); ++i) {
        position[order[i]] = i;
      }
      std::vector<FlameItem> sorted;
      sorted.reserve(row.size());
      for (const auto i : order) {
        sorted.push_back(std::move(row[i]));
      }
      if (firstRow) {
        for (auto& item : sorted) {
          if (!item.more) {
            item.top = position[item.top];
          }
        }
      }
      row = std::move(sorted);
      for (auto& parent : next) {
        parent.item = position[parent.item];
      }
    }
  } // namespace

  FlameAhead flameAhead(
      const Tree& tree, const LayoutSettings& settings, NodeId cwd, double x0, double x1,
      const FlameAheadOptions& options
  ) {
    FlameAhead result;
    const std::size_t levels = std::min(options.levels, result.rows.size());
    std::vector<Parent> frontier{{.node = cwd, .x0 = x0, .x1 = x1, .item = kNoFlameItem}};
    for (std::size_t level = 0; level < levels; ++level) {
      std::ranges::stable_sort(frontier, std::greater{}, [](const Parent& parent) { return parent.x1 - parent.x0; });
      const double min = options.min ? *options.min : kMinSegment[level];
      std::vector<Parent> next;
      for (const auto& parent : frontier) {
        const std::size_t childCount = children(tree, parent.node).size();
        if (parent.x1 - parent.x0 < 2 * min || childCount == 0) {
          continue;
        }
        if (level > 0 && result.spent + childCount > options.budget) {
          result.truncated = true;
          continue;
        }
        result.spent += childCount;
        layParent(tree, settings, options, level, parent, result, next);
      }
      sortRow(result.rows[level], next, level == 0);
      frontier = std::move(next);
    }
    return result;
  }

  std::vector<FlameItem> flameStratum(
      const Tree& tree, const LayoutSettings& settings, NodeId cwd, std::span<const NodeId> ancestors, std::size_t k,
      double x0, double x1, const FlameStratumOptions& options
  ) {
    std::vector<FlameItem> items;
    if (k == 0 || k > ancestors.size()) {
      return items;
    }
    const NodeId onPath = k == 1 ? cwd : ancestors[k - 2];
    const NodeId owner = ancestors[k - 1];
    if (!hasNode(tree, onPath) || !hasNode(tree, owner)) {
      return items;
    }
    auto folders = folderShares(tree, settings, owner);
    auto found = std::ranges::find(folders.dirs, onPath);
    std::size_t pin = static_cast<std::size_t>(found - folders.dirs.begin());
    if (found == folders.dirs.end()) {
      // A hidden folder on the way is not among the visible entries: it is shown first with an equal share.
      const auto total = static_cast<double>(folders.dirs.size() + 1);
      for (auto& share : folders.shares) {
        share = share * (total - 1) / total;
      }
      folders.dirs.insert(folders.dirs.begin(), onPath);
      folders.shares.insert(folders.shares.begin(), 1 / total);
      pin = 0;
    }
    std::vector<double> want;
    if (options.fonts) {
      want = k <= kFlameNamedStrata ? wantFor(tree, folders.dirs, options.fonts->ancestor)
                                    : std::vector<double>(std::min(folders.dirs.size(), kWantRows), 0.0);
    }
    const FlameRowOptions rowOptions{
        .min = options.min ? *options.min : (k <= kFlameNamedStrata ? kMinSegment[0] : kMinSegment[1]),
        .cap = options.cap,
        .pin = pin,
        .pinMin = std::min(options.pinMin, x1 - x0),
        .want = want,
    };
    const auto split = flameRowLayout(folders.shares, x0, x1, rowOptions);
    items.reserve(split.segs.size() + 1);
    for (const auto& seg : split.segs) {
      FlameItem item;
      item.node = folders.dirs[seg.index];
      item.level = k;
      item.x = seg.x;
      item.w = seg.w;
      item.pinned = seg.index == pin;
      items.push_back(std::move(item));
    }
    if (split.more) {
      FlameItem more;
      more.more = true;
      more.owner = owner;
      for (const auto index : split.more->indices) {
        more.list.push_back(folders.dirs[index]);
      }
      more.level = k;
      more.x = split.more->x;
      more.w = split.more->w;
      items.push_back(std::move(more));
    }
    return items;
  }

  std::vector<FlameItem> flameFarLine(std::span<const NodeId> ancestors, std::size_t strata, double x0, double x1) {
    std::vector<FlameItem> cells;
    if (strata == 0 || strata > ancestors.size()) {
      return cells;
    }
    const std::size_t count = ancestors.size() - (strata - 1);
    const double w = (x1 - x0) / static_cast<double>(count);
    cells.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      FlameItem cell;
      cell.node = ancestors[ancestors.size() - 1 - i];
      cell.level = strata + 1;
      cell.far = true;
      cell.x = x0 + static_cast<double>(i) * w;
      cell.w = w;
      cells.push_back(std::move(cell));
    }
    return cells;
  }
} // namespace luciole::workbench
