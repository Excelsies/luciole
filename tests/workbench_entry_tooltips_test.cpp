#include "test_check.h"
#include "ui/workbench/entry_tooltip.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <memory>
#include <string>
#include <variant>
#include <vector>

// The presenter asks for a tooltip on every layout pass while the pointer rests: the answer is worked out once and
// given again until the snapshot, the shape or the settings change.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;

  std::shared_ptr<const Tree> snapshot() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 300, .items = 2},
         {.parent = 0, .name = "a", .folder = true, .bytes = 200, .items = 1},
         {.parent = 0, .name = "b", .folder = true, .bytes = 100, .items = 1},
         {.parent = 1, .name = "locked", .folder = true, .unreadable = true, .truncated = true}},
        0
    );
    tree.nodes[0].lowerBound = true;
    tree.nodes[1].lowerBound = true;
    tree.nodes[3].lowerBound = true;
    return std::make_shared<const Tree>(std::move(tree));
  }

  ShapeId piece(NodeId node) { return {.node = node, .role = shapeRole(SceneRole::Piece)}; }

  std::string textOf(const TooltipContent& content) {
    const auto* text = std::get_if<std::string>(&content);
    return text != nullptr ? *text : std::string{};
  }

  void aSecondAskWorksNothingOut() {
    const auto tree = snapshot();
    const LayoutSettings settings;
    EntryTooltips tips;
    const auto first = tips.entry(tree, &settings, piece(1));
    TEST_CHECK(textOf(first).ends_with("\nNot counted: 1 unreadable folder (locked)."));
    TEST_CHECK(tips.worked() == 1);
    for (int pass = 0; pass < 50; ++pass) {
      TEST_CHECK(textOf(tips.entry(tree, &settings, piece(1))) == textOf(first));
    }
    TEST_CHECK(tips.worked() == 1);
  }

  void aChangeWorksTheAnswerOutAgain() {
    const auto tree = snapshot();
    LayoutSettings settings;
    EntryTooltips tips;
    (void)tips.entry(tree, &settings, piece(1));
    (void)tips.entry(tree, &settings, piece(2));
    TEST_CHECK(tips.worked() == 2);
    settings.showHidden = true;
    (void)tips.entry(tree, &settings, piece(2));
    TEST_CHECK(tips.worked() == 3);
    (void)tips.entry(tree, nullptr, piece(2));
    TEST_CHECK(tips.worked() == 4);
    // A new snapshot, even an equal one, is another answer.
    const auto other = snapshot();
    (void)tips.entry(other, nullptr, piece(2));
    TEST_CHECK(tips.worked() == 5);
  }

  void shapesThatStandForNoEntryAreNotWorkedOut() {
    const auto tree = snapshot();
    EntryTooltips tips;
    const ShapeId detail{.node = 1, .role = shapeRole(SceneRole::Detail)};
    TEST_CHECK(std::holds_alternative<std::monostate>(tips.entry(tree, nullptr, detail)));
    TEST_CHECK(std::holds_alternative<std::monostate>(tips.entry(nullptr, nullptr, piece(1))));
    TEST_CHECK(tips.worked() == 0);
  }

  void aMergedPieceIsKeptToo() {
    const auto tree = snapshot();
    EntryTooltips tips;
    const ShapeId more{.node = 0, .role = shapeRole(SceneRole::More)};
    const std::vector<NodeId> held{1, 2};
    const auto first = tips.merged(tree, more, held);
    TEST_CHECK(textOf(first).starts_with("2 more entries\nat least 300 bytes\nNot counted:"));
    TEST_CHECK(textOf(tips.merged(tree, more, held)) == textOf(first));
    TEST_CHECK(tips.worked() == 1);
    // A layout that merges another set of entries is another answer.
    const std::vector<NodeId> fewer{2};
    TEST_CHECK(tips.merged(tree, more, fewer).index() == 0);
    TEST_CHECK(tips.worked() == 2);
  }

  // Other settings can change which entries a piece merges while its count and its first and last entry stay: the
  // answer belongs to the whole list.
  void aMergedPieceKeyIsItsWholeList() {
    auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "r", .folder = true, .bytes = 1000, .items = 4},
         {.parent = 0, .name = "a", .folder = true, .bytes = 100},
         {.parent = 0, .name = "b", .folder = true, .bytes = 200},
         {.parent = 0, .name = "c", .folder = true, .bytes = 300},
         {.parent = 0, .name = "d", .folder = true, .bytes = 400}},
        0
    );
    tree.nodes[2].lowerBound = true;
    tree.nodes[3].lowerBound = true;
    const auto shared = std::make_shared<const Tree>(std::move(tree));
    EntryTooltips tips;
    const ShapeId more{.node = 0, .role = shapeRole(SceneRole::More)};
    const std::vector<NodeId> withB{1, 2, 4};
    const std::vector<NodeId> withC{1, 3, 4};
    const auto first = textOf(tips.merged(shared, more, withB));
    TEST_CHECK(first.starts_with("3 more entries\nat least 700 bytes\n"));
    TEST_CHECK(textOf(tips.merged(shared, more, withB)) == first);
    TEST_CHECK(tips.worked() == 1);
    // Same count, same first and last, another middle: another size.
    const auto second = textOf(tips.merged(shared, more, withC));
    TEST_CHECK(second.starts_with("3 more entries\nat least 800 bytes\n"));
    TEST_CHECK(tips.worked() == 2);
    // Other entries because other settings changed the layout: the same, a new answer.
    const std::vector<NodeId> exact{1, 4};
    TEST_CHECK(std::holds_alternative<std::monostate>(tips.merged(shared, more, exact)));
    TEST_CHECK(tips.worked() == 3);
  }
} // namespace

int main() {
  aSecondAskWorksNothingOut();
  aChangeWorksTheAnswerOutAgain();
  shapesThatStandForNoEntryAreNotWorkedOut();
  aMergedPieceIsKeptToo();
  aMergedPieceKeyIsItsWholeList();
  return 0;
}
