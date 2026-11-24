#include "browser/carried_sizes.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace {
  using luciole::FoldedChildren;
  using luciole::workbench::kNoNode;
  using luciole::workbench::Node;
  using luciole::workbench::NodeId;

  // What `previous` showed for the folder `name`.
  struct Shown {
    double bytes = 0;
    double files = 0;
    double items = 0;
    double modified = 0;
    bool lowerBound = false;
    bool fromNode = false;
  };

  std::optional<Shown> shownBy(const luciole::WorkbenchSnapshot& previous, const std::string& name) {
    if (previous.folderSizeText(name).empty()) {
      return std::nullopt;
    }
    if (const auto id = previous.node(name); id != kNoNode) {
      const auto& node = previous.tree.nodes[id];
      return Shown{node.bytes, node.files, node.items, node.modified, node.lowerBound || node.partial, true};
    }
    const auto* entry = previous.folded ? previous.folded->find(name) : nullptr;
    if (entry == nullptr) {
      return std::nullopt;
    }
    return Shown{static_cast<double>(entry->apparentBytes),        0,    0, 0,
                 (entry->flags & FoldedChildren::kTruncated) != 0, false};
  }

  double gain(double now, double was) { return now - was; }
} // namespace

void luciole::carryPreviousSizes(
    workbench::Tree& tree, std::shared_ptr<const FoldedChildren>& folded, const WorkbenchSnapshot& previous
) {
  if (!workbench::hasNode(tree, tree.root)) {
    return;
  }
  double added = 0;
  double addedFiles = 0;
  bool carriedAny = false;
  bool notCarried = false;
  bool lower = false; // a row that is a lower bound
  double addedToRemainder = 0;
  const auto root = tree.root;
  NodeId remainder = kNoNode;
  for (const auto child : workbench::children(tree, root)) {
    auto& node = tree.nodes[child];
    if (node.name.empty() && node.folder) {
      remainder = child;
    } else if (node.notEntered) {
      const auto shown = shownBy(previous, node.name);
      notCarried = notCarried || !shown;
      if (shown) {
        carriedAny = true;
        lower = lower || shown->lowerBound;
        addedFiles += shown->fromNode ? gain(shown->files, node.files) : 0;
        added += gain(shown->bytes, node.bytes);
        node.bytes = shown->bytes;
        node.files = shown->fromNode ? shown->files : node.files;
        node.items = shown->fromNode ? shown->items : node.items;
        node.modified = shown->fromNode ? std::max(node.modified, shown->modified) : node.modified;
        node.lowerBound = shown->lowerBound;
        node.notEntered = false;
        node.partial = false;
      }
    } else {
      lower = lower || node.lowerBound;
    }
  }
  if (folded) {
    std::shared_ptr<FoldedChildren> copy;
    for (std::size_t i = 0; i < folded->entries.size(); ++i) {
      if ((folded->entries[i].flags & FoldedChildren::kNotEntered) == 0) {
        lower = lower || FoldedChildren::cut(folded->entries[i].flags);
        continue;
      }
      const auto shown = shownBy(previous, std::string(folded->nameOf(i)));
      notCarried = notCarried || !shown;
      if (!shown) {
        continue;
      }
      carriedAny = true;
      lower = lower || shown->lowerBound;
      copy = copy ? copy : std::make_shared<FoldedChildren>(*folded);
      auto& entry = copy->entries[i];
      addedToRemainder += gain(shown->bytes, static_cast<double>(entry.apparentBytes));
      entry.apparentBytes = static_cast<std::uint64_t>(shown->bytes);
      entry.flags = shown->lowerBound ? FoldedChildren::kTruncated : 0;
    }
    if (copy) {
      folded = std::move(copy);
    }
  }
  if (remainder != kNoNode) {
    tree.nodes[remainder].bytes += addedToRemainder;
  }
  auto& top = tree.nodes[root];
  top.bytes += added + addedToRemainder;
  top.files += addedFiles;
  if (carriedAny && !notCarried) { // every unknown folder is known again: the root is as exact as the rows
    top.lowerBound = lower || previous.tree.nodes[previous.tree.root].lowerBound;
  }
}
