#include "test_check.h"
#include "workbench/tree.h"
#include "workbench_fixture.h"

#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::test::referenceTree;

  std::vector<NodeId> ids(std::span<const NodeId> span) { return {span.begin(), span.end()}; }

  void testChildren() {
    const auto tree = referenceTree();
    TEST_CHECK((ids(children(tree, 0)) == std::vector<NodeId>{1, 2, 3, 4}));
    TEST_CHECK((ids(children(tree, 1)) == std::vector<NodeId>{5, 6}));
    TEST_CHECK((ids(children(tree, 7)) == std::vector<NodeId>{9}));
    TEST_CHECK(children(tree, 4).empty());
    TEST_CHECK(children(tree, 9).empty());
  }

  void testParentChainAndDepth() {
    const auto tree = referenceTree();
    TEST_CHECK((parentChain(tree, 9) == std::vector<NodeId>{7, 5, 1, 0}));
    TEST_CHECK(parentChain(tree, 0).empty());
    TEST_CHECK(depth(tree, 0) == 0);
    TEST_CHECK(depth(tree, 1) == 1);
    TEST_CHECK(depth(tree, 7) == 3);
    TEST_CHECK(depth(tree, 9) == 4);
  }

  void testParentAndPathFromTop() {
    auto tree = referenceTree();
    TEST_CHECK(parentOf(tree, 9) == 7);
    TEST_CHECK(parentOf(tree, 0) == kNoNode);
    TEST_CHECK(parentOf(tree, 99) == kNoNode);
    TEST_CHECK((pathFromTop(tree, 9) == std::vector<NodeId>{0, 1, 5, 7, 9}));
    TEST_CHECK((pathFromTop(tree, 0) == std::vector<NodeId>{0}));
    TEST_CHECK(pathFromTop(tree, 99).empty());
    tree.nodes[0].parent = 9;
    TEST_CHECK(pathFromTop(tree, 9).size() == tree.nodes.size() + 1);
  }

  void testVisibleEntry() {
    const auto tree = referenceTree();
    TEST_CHECK(visibleEntry(tree.nodes[2], false));
    TEST_CHECK(!visibleEntry(tree.nodes[3], false));
    TEST_CHECK(visibleEntry(tree.nodes[3], true));
  }

  void testInvalidIdsAndBrokenLinks() {
    auto tree = referenceTree();
    TEST_CHECK(!hasNode(tree, kNoNode));
    TEST_CHECK(!hasNode(tree, 10));
    TEST_CHECK(hasNode(tree, 9));
    TEST_CHECK(children(tree, kNoNode).empty());
    TEST_CHECK(children(tree, 99).empty());
    TEST_CHECK(parentChain(tree, 99).empty());
    TEST_CHECK(depth(tree, kNoNode) == 0);

    tree.nodes[4].firstChild = 8;
    tree.nodes[4].childCount = 5;
    TEST_CHECK(children(tree, 4).empty());
    tree.nodes[4].firstChild = 0xFFFFFFFFU;
    tree.nodes[4].childCount = 2;
    TEST_CHECK(children(tree, 4).empty());

    tree.nodes[2].parent = 77;
    TEST_CHECK(parentChain(tree, 2).empty());
    TEST_CHECK(depth(tree, 2) == 0);
  }

  void testCycleStops() {
    auto tree = referenceTree();
    tree.nodes[0].parent = 9;
    const auto chain = parentChain(tree, 9);
    TEST_CHECK(chain.size() == tree.nodes.size());
    TEST_CHECK(depth(tree, 9) == tree.nodes.size());
  }

  void testFolderFlags() {
    const Node plain;
    TEST_CHECK(!plain.unreadable);
    TEST_CHECK(!plain.truncated);
    TEST_CHECK(!plain.otherFilesystem);

    const auto reference = referenceTree();
    for (const auto& node : reference.nodes) {
      TEST_CHECK(!node.unreadable && !node.truncated && !node.otherFilesystem);
    }

    const auto tree = luciole::test::workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true},
            {.parent = 0, .name = "locked", .folder = true, .unreadable = true},
            {.parent = 0, .name = "big", .folder = true, .truncated = true},
            {.parent = 0, .name = "mnt", .folder = true, .otherFilesystem = true},
        },
        0
    );
    TEST_CHECK(tree.nodes[1].unreadable && !tree.nodes[1].truncated && !tree.nodes[1].otherFilesystem);
    TEST_CHECK(!tree.nodes[2].unreadable && tree.nodes[2].truncated && !tree.nodes[2].otherFilesystem);
    TEST_CHECK(!tree.nodes[3].unreadable && !tree.nodes[3].truncated && tree.nodes[3].otherFilesystem);
  }

  void testFolderState() {
    Node node;
    TEST_CHECK(folderState(node) == FolderState::Listed);
    node.truncated = true;
    TEST_CHECK(folderState(node) == FolderState::NotExpanded);
    node.otherFilesystem = true;
    TEST_CHECK(folderState(node) == FolderState::OtherFilesystem);
    node.unreadable = true;
    TEST_CHECK(folderState(node) == FolderState::Unreadable);
  }

  void testEmptyTree() {
    const Tree tree;
    TEST_CHECK(tree.root == kNoNode);
    TEST_CHECK(children(tree, 0).empty());
    TEST_CHECK(parentChain(tree, 0).empty());
    TEST_CHECK(depth(tree, 0) == 0);
  }
} // namespace

int main() {
  testChildren();
  testParentChainAndDepth();
  testParentAndPathFromTop();
  testVisibleEntry();
  testInvalidIdsAndBrokenLinks();
  testCycleStops();
  testFolderFlags();
  testFolderState();
  testEmptyTree();
  std::println("workbench tree checks passed");
  return 0;
}
