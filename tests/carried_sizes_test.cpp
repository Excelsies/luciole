#include "browser/carried_sizes.h"
#include "test_check.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
  using luciole::FoldedChildren;
  using luciole::Location;
  using luciole::WorkbenchSnapshot;
  using luciole::workbench::kNoNode;
  using luciole::workbench::Node;
  using luciole::workbench::NodeId;
  using luciole::workbench::Tree;

  struct Child {
    std::string name;
    Node node;
  };

  // A snapshot of the folder /r: the children as nodes, a nameless remainder node when `rest` is given, and the
  // folded list.
  WorkbenchSnapshot snapshotOf(
      const std::vector<Child>& children, std::shared_ptr<const FoldedChildren> folded = nullptr, double rest = -1,
      const std::string& place = "/r"
  ) {
    Tree tree;
    tree.root = 0;
    tree.nodes.push_back({.name = "r", .folder = true});
    std::vector<luciole::WorkbenchShare> shares;
    std::vector<NodeId> ids;
    for (const auto& child : children) {
      auto node = child.node;
      node.name = child.name;
      node.folder = true;
      node.parent = 0;
      tree.nodes[0].bytes += node.bytes;
      ids.push_back(static_cast<NodeId>(tree.nodes.size()));
      shares.push_back({.name = child.name, .id = ids.back(), .share = 0});
      tree.nodes.push_back(node);
    }
    if (rest >= 0) {
      ids.push_back(static_cast<NodeId>(tree.nodes.size()));
      tree.nodes.push_back({.name = "", .parent = 0, .bytes = rest, .folder = true});
      tree.nodes[0].bytes += rest;
    }
    tree.nodes[0].firstChild = 0;
    tree.nodes[0].childCount = static_cast<std::uint32_t>(ids.size());
    tree.children = ids;
    std::ranges::sort(shares, {}, &luciole::WorkbenchShare::name);
    return {
        .serial = 1,
        .location = Location::fromPath(place),
        .settings = {},
        .tree = std::move(tree),
        .jump = {},
        .jumpLocations = {},
        .shares = std::move(shares),
        .total = 0,
        .error = std::nullopt,
        .notMeasured = false,
        .folded = std::move(folded),
    };
  }

  std::shared_ptr<const FoldedChildren>
  listOf(const std::vector<std::tuple<std::string, double, std::uint8_t>>& items) {
    auto list = std::make_shared<FoldedChildren>();
    for (const auto& [name, bytes, flags] : items) {
      list->names += name;
      list->entries.push_back(
          {.apparentBytes = static_cast<std::uint64_t>(bytes),
           .allocatedBytes = 0,
           .nameEnd = static_cast<std::uint32_t>(list->names.size()),
           .flags = flags}
      );
    }
    return list;
  }

  constexpr std::uint8_t kNever = FoldedChildren::kNotEntered | FoldedChildren::kTruncated;

  // A tree node for a folder the walk never entered.
  Child never(const std::string& name) {
    Node node;
    node.lowerBound = true;
    node.notEntered = true;
    return {name, node};
  }
  Child sized(const std::string& name, double bytes, bool cut = false) {
    Node node;
    node.bytes = bytes;
    node.files = 3;
    node.items = 3;
    node.lowerBound = cut;
    return {name, node};
  }

  // The cell of a folder, and the root's size, in the snapshot that `tree` and `folded` make.
  std::string cellOf(const Tree& tree, const std::shared_ptr<const FoldedChildren>& folded, const std::string& name) {
    auto snapshot = snapshotOf({});
    snapshot.tree = tree;
    snapshot.folded = folded;
    for (NodeId id = 1; id < tree.nodes.size(); ++id) {
      if (!tree.nodes[id].name.empty()) {
        snapshot.shares.push_back({.name = tree.nodes[id].name, .id = id, .share = 0});
      }
    }
    std::ranges::sort(snapshot.shares, {}, &luciole::WorkbenchShare::name);
    return snapshot.folderSizeText(name);
  }

  // Folders shown with a size before keep it, whether the new tree names them or folds them, and whether the old one
  // named or folded them.
  void sizesCarried() {
    const auto previous = snapshotOf(
        {sized("a", 1000), sized("b", 2048, true), sized("c", 4096)},
        listOf({{"d", 3072, 0}, {"e", 1024, FoldedChildren::kTruncated}, {"f", 0, kNever}}), 5000
    );
    auto tree = snapshotOf(
                    {never("a"), never("b"), never("c"), sized("x", 10)},
                    listOf({{"d", 0, kNever}, {"e", 0, kNever}, {"f", 0, kNever}, {"g", 0, kNever}}), 0
    )
                    .tree;
    auto folded = listOf({{"d", 0, kNever}, {"e", 0, kNever}, {"f", 0, kNever}, {"g", 0, kNever}});
    const double before = tree.nodes[tree.root].bytes;
    luciole::carryPreviousSizes(tree, folded, previous);
    TEST_CHECK(cellOf(tree, folded, "a") == "1000 bytes");
    TEST_CHECK(cellOf(tree, folded, "b") == "≥ 2.0 KiB");
    TEST_CHECK(cellOf(tree, folded, "c") == "4.0 KiB");
    TEST_CHECK(cellOf(tree, folded, "x") == "10 bytes");
    TEST_CHECK(cellOf(tree, folded, "d") == "3.0 KiB");
    TEST_CHECK(cellOf(tree, folded, "e") == "≥ 1.0 KiB");
    TEST_CHECK(cellOf(tree, folded, "f").empty() && cellOf(tree, folded, "g").empty()); // nothing was shown
    const auto& a = tree.nodes[tree.children[tree.nodes[tree.root].firstChild]];
    TEST_CHECK(a.name == "a" && a.bytes == 1000 && !a.notEntered && !a.lowerBound);
    // The root and the folded remainder moved by what the folders gained: 1000 + 2048 + 4096 and 3072 + 1024.
    TEST_CHECK(tree.nodes[tree.root].bytes == before + 1000 + 2048 + 4096 + 3072 + 1024);
    const auto remainder = tree.children.back();
    TEST_CHECK(tree.nodes[remainder].name.empty() && tree.nodes[remainder].bytes == 3072 + 1024);
  }

  // A folder the old snapshot also knew nothing of stays blank.
  void blankStaysBlank() {
    auto tree = snapshotOf({never("a")}).tree;
    std::shared_ptr<const FoldedChildren> none;
    luciole::carryPreviousSizes(tree, none, snapshotOf({never("a")}));
    TEST_CHECK(tree.nodes[1].notEntered && tree.nodes[1].bytes == 0);
  }
} // namespace

int main() {
  sizesCarried();
  blankStaysBlank();
  return 0;
}
