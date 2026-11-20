#include "test_check.h"
#include "workbench/entry_text.h"
#include "workbench/format.h"
#include "workbench/measure.h"
#include "workbench_fixture.h"

#include <string>
#include <vector>

// The tooltip text of an entry: name, facts, and the sentence that says why an amount reads "at least". Every view
// that has a tooltip for an entry takes it from here.
namespace {
  using namespace luciole::workbench;
  using luciole::test::workbenchTree;

  constexpr double kGiB = 1073741824.0;

  // "/" (3 items) holding proc (a mount point), usr (exact) and home (a file of 2 GiB). Ids: 0, 1 proc, 2 usr, 3 home.
  Tree system() {
    auto tree = workbenchTree(
        {{.parent = -1, .name = "/", .folder = true, .bytes = 6 * kGiB, .items = 3, .files = 40, .modified = 100},
         {.parent = 0,
          .name = "proc",
          .folder = true,
          .bytes = 0,
          .modified = 100,
          .truncated = true,
          .otherFilesystem = true},
         {.parent = 0, .name = "usr", .folder = true, .bytes = 4 * kGiB, .items = 9, .files = 30, .modified = 100},
         {.parent = 0, .name = "home.img", .bytes = 2 * kGiB, .modified = 100}},
        3700
    );
    tree.nodes[0].lowerBound = true;
    tree.nodes[1].lowerBound = true;
    return tree;
  }

  void anExactFolderHasNoSentence() {
    auto tree = system();
    tree.nodes[0].lowerBound = false;
    tree.nodes[1].lowerBound = false;
    LayoutSettings settings;
    settings.emphasis = 1; // shares follow the sizes
    TEST_CHECK(
        entryText(entryFacts(tree, 2, &settings))
        == "usr\n4.0\u00A0GiB  ·  67%  ·  9 items  ·  30 files  ·  modified 1 h ago"
    );
    TEST_CHECK(
        entryText(entryFacts(tree, 3, &settings)) == "home.img\n2.0\u00A0GiB  ·  33%  ·  IMG  ·  modified 1 h ago"
    );
  }

  // The share of an entry beside a sibling that is a lower bound is approximate.
  void aLowerBoundSiblingMakesTheShareApproximate() {
    const auto tree = system();
    const LayoutSettings settings;
    TEST_CHECK(entryFacts(tree, 2, &settings).share.starts_with("about "));
    auto exact = system();
    exact.nodes[1].lowerBound = false;
    TEST_CHECK(!entryFacts(exact, 2, &settings).share.starts_with("about "));
  }

  void aLowerBoundSaysWhyOnALineOfItsOwn() {
    const auto tree = system();
    const auto text = entryText(entryFacts(tree, 0));
    TEST_CHECK(
        text.starts_with("/\nat least 6.0 GiB  ·  at least 3 items  ·  at least 40 files  ·  modified 1 h ago\n")
    );
    TEST_CHECK(text.ends_with("\nNot counted: proc is on another filesystem."));
    // A mount point at zero reads "not measured" once, and its own reason is the sentence, not a tag as well.
    const auto proc = entryText(entryFacts(tree, 1));
    TEST_CHECK(proc == "proc\nnot measured  ·  modified 1 h ago\nNot counted: proc is on another filesystem.");
  }

  void aTagThatTheSentenceTellsIsNotRepeated() {
    auto tree = system();
    tree.nodes[2].unreadable = true;
    tree.nodes[2].lowerBound = true;
    const auto text = entryText(entryFacts(tree, 2));
    TEST_CHECK(text.find("unreadable") == text.rfind("unreadable"));
    TEST_CHECK(text.ends_with("\nNot counted: 1 unreadable folder (usr)."));
  }

  // "Not expanded" tells the snapshot, not the walk, so it stays beside the sentence.
  void notExpandedStays() {
    auto tree = system();
    tree.nodes[2].truncated = true;
    tree.nodes[2].lowerBound = true;
    const auto text = entryText(entryFacts(tree, 2));
    TEST_CHECK(
        text.contains("not expanded") && text.ends_with("\nNot counted: some folders below were not counted in full.")
    );
  }

  void measuringHasNoSentence() {
    auto tree = system();
    tree.nodes[0].partial = true;
    const auto text = entryText(entryFacts(tree, 0));
    TEST_CHECK(text.starts_with("/\n6.0\u00A0GiB") && !text.contains("Not counted"));
  }

  // A merged piece says how many entries it holds and their summed size, and only when that size reads "at least".
  void aMergedPieceSaysWhyItsTotalIsALowerBound() {
    const auto tree = system();
    const std::vector<NodeId> held{1, 2, 3};
    TEST_CHECK(
        mergedText(tree, held) == "3 more entries\nat least 6.0\u00A0GiB\nNot counted: proc is on another filesystem."
    );
    const std::vector<NodeId> exact{2, 3};
    TEST_CHECK(mergedText(tree, exact).empty());
    // Entries still being measured read "measuring…", which says it already.
    auto running = system();
    running.nodes[2].partial = true;
    TEST_CHECK(mergedText(running, held).empty());
    TEST_CHECK(mergedText(tree, std::vector<NodeId>{}).empty());
  }

  // A folder above the open one has no share: its siblings are only listed, and a share among them says nothing.
  void aFolderAboveTheOpenOneHasNoShare() {
    auto tree = system();
    tree.root = 2;
    tree.ancestors = {0};
    const LayoutSettings settings;
    TEST_CHECK(entryFacts(tree, 0, &settings).share.empty());
    TEST_CHECK(!entryFacts(tree, 3, &settings).share.empty());
    TEST_CHECK(
        entryFacts(tree, 0, &settings).notCounted.starts_with("Not counted: measuring this folder stopped at a limit")
    );
  }

  void anUnknownIdHasNothing() {
    const auto tree = system();
    TEST_CHECK(entryText(entryFacts(tree, kNoNode)).empty());
    TEST_CHECK(entryText(entryFacts(tree, 99)).empty());
  }

  void aTopWithoutAnameIsASlash() {
    auto tree = system();
    tree.nodes[0].name.clear();
    TEST_CHECK(entryText(entryFacts(tree, 0)).starts_with("/\n"));
  }

  void shareTextRounds() {
    TEST_CHECK(shareText(0.48) == "48%");
    TEST_CHECK(shareText(0.0123) == "1.2%");
    TEST_CHECK(shareText(1e-7) == "<0.1%");
    TEST_CHECK(shareText(-1).empty());
  }
} // namespace

int main() {
  anExactFolderHasNoSentence();
  aLowerBoundSiblingMakesTheShareApproximate();
  aLowerBoundSaysWhyOnALineOfItsOwn();
  aTagThatTheSentenceTellsIsNotRepeated();
  notExpandedStays();
  measuringHasNoSentence();
  aMergedPieceSaysWhyItsTotalIsALowerBound();
  aFolderAboveTheOpenOneHasNoShare();
  anUnknownIdHasNothing();
  aTopWithoutAnameIsASlash();
  shareTextRounds();
  return 0;
}
