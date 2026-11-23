#include "core/input/key_modifiers.h"
#include "ui/workbench/ancestor_strips.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench_view_rig.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <print>
#include <set>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

// What the Workbench view asks of the page beyond plain clicks: new tabs, the list of a merged piece, where the
// keyboard focus goes after a strip's key, the columns reopening where the user was, and the motion of a change of
// folder.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;

  constexpr std::size_t kMany = 900;

  void middleClicksAskForNewTabs() {
    Rig rig;
    rig.settle();
    rig.held = KeyMod::Shift;
    const auto asked = [&rig](std::size_t count, const Location& where) {
      TEST_CHECK(rig.requests.size() == count && rig.gone.size() == count && rig.gone.back() == where);
      TEST_CHECK(rig.requests.back().button == BTN_MIDDLE && rig.requests.back().modifiers == KeyMod::Shift);
    };
    rig.click(middle(*rig.jumpRow("mid/sub")), BTN_MIDDLE);
    asked(1, rig.fixture.at("mid/sub"));
    rig.click(middle(*rig.columnRow(0, "deep")), BTN_MIDDLE);
    asked(2, rig.fixture.at("big/deep"));
    rig.click(middle(*rig.columnRow(0, "b.bin")), BTN_MIDDLE); // a file does nothing, as in the list
    TEST_CHECK(rig.requests.size() == 2);
    auto& strips = rig.view->center().strips();
    TEST_CHECK(rig.focuses.empty());
    rig.click(middle(*strips.nearest()), BTN_MIDDLE);
    asked(3, Location::fromPath(rig.fixture.base.native()));
    rig.frame();
    // the press took the focus to the strip: it goes back to the list
    TEST_CHECK(rig.focuses.size() == 1 && rig.focuses.back() == rig.list);
    // nothing else moved: no pin, no Jump Ahead cursor
    TEST_CHECK(!rig.state.pinned(0) && !rig.state.locked() && !rig.view->jumpList().cursor());
  }

  void stripKeyLeavesTheFocusOnTheStrips() {
    Rig rig;
    rig.view->onFocus = [&rig](InputArea* area) {
      rig.focuses.push_back(area);
      rig.dispatcher.setFocus(area);
    };
    rig.open("big/deep");
    rig.settle();
    auto& strips = rig.view->center().strips();
    rig.dispatcher.setFocus(strips.nearest());
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(rig.gone.size() == 1 && rig.gone.back() == rig.fixture.at("big"));
    TEST_CHECK(rig.requests.back().button == 0 && rig.requests.back().modifiers == 0);
    rig.frame();
    TEST_CHECK(rig.focuses.empty()); // the page has not entered the folder yet
    rig.open("big");
    rig.frame();
    TEST_CHECK(rig.focuses.size() == 1 && rig.focuses.back() == strips.nearest());
    TEST_CHECK(strips.label(strips.count() - 1)->text() == "root");
    // a click on a strip gives the focus to the list
    rig.click(middle(*strips.nearest()));
    rig.frame();
    TEST_CHECK(rig.focuses.size() == 2 && rig.focuses.back() == rig.list);
  }

  // A point inside the hittable "+N more" piece of `folder`, in scene coordinates.
  std::optional<Point> morePiece(const PartView& part, wb::NodeId folder) {
    for (const auto& shape : part.shapeView().shapes()) {
      if (shape.id.node == folder && shape.hittable && wb::sceneRole(shape.id.role) == wb::SceneRole::More) {
        Point at;
        Node::absolutePosition(&part.shapeView(), at.x, at.y);
        return Point{
            .x = at.x + shape.rect.x + (shape.rect.width / 2), .y = at.y + shape.rect.y + (shape.rect.height / 2)
        };
      }
    }
    return std::nullopt;
  }

  // many/ holds f0000.bin to f0899.bin, f<i> of i + 1 bytes, and sub/ with one byte below it.
  void mergedPieceListsWhatItHolds() {
    Rig rig;
    for (std::size_t i = 0; i < kMany; ++i) {
      Fixture::write(rig.fixture.root / "many" / std::format("f{:04}.bin", i), i + 1);
    }
    Fixture::write(rig.fixture.root / "many" / "sub" / "s.bin", 1);
    rig.open("many");
    rig.settle();
    // A window whose Nest has no room for every entry the tree holds.
    rig.resize(700, 420);
    rig.settle();
    const auto& nest = rig.view->instrument();
    const auto& tree = rig.model.snapshot()->tree;
    const auto piece = morePiece(nest, tree.root);
    TEST_CHECK(piece.has_value());
    rig.click(*piece);
    TEST_CHECK(rig.mores.size() == 1 && rig.requests.empty());
    const auto& more = rig.mores.back();
    TEST_CHECK(more.pane == WorkbenchPane::Instrument && more.folder == rig.fixture.at("many"));
    TEST_NEAR(more.sceneX, piece->x, 0.5F);
    // the piece holds the entries the Nest draws no tile for, the lightest ones, and those the tree folds
    std::set<std::string> drawn;
    double folded = 0; // entries of remainders drawn as their own tile
    for (const auto& shape : nest.shapeView().shapes()) {
      if (wb::hasNode(tree, shape.id.node) && tree.nodes[shape.id.node].parent == tree.root) {
        const auto& node = tree.nodes[shape.id.node];
        if (node.name.empty()) {
          folded += node.items;
        } else {
          drawn.insert(node.name);
        }
      }
    }
    std::println(
        "{} entries: {} drawn, {} folded and drawn, {} merged, {} listed", kMany + 1, drawn.size(), folded, more.count,
        more.listed.size()
    );
    TEST_CHECK(static_cast<double>(drawn.size() + more.count) + folded == static_cast<double>(kMany + 1));
    TEST_CHECK(!more.listed.empty() && more.listed.size() <= WorkbenchView::kMoreListed);
    for (std::size_t i = 0; i < more.listed.size(); ++i) {
      const auto& listed = more.listed[i];
      TEST_CHECK(!drawn.contains(listed.name) && !listed.value.empty());
      TEST_CHECK(listed.entry.location == rig.fixture.at("many").child(listed.name));
      TEST_CHECK(i == 0 || listed.name < more.listed[i - 1].name || listed.name == "sub"); // heaviest first
    }
    TEST_CHECK(more.listed.front().name < *std::ranges::min_element(drawn));
    // Enter on the "+N more" stop lists them at the piece
    rig.dispatcher.setFocus(&nest.shapeView().inputArea());
    rig.dispatcher.keyEvent(XKB_KEY_End, 0, 0, true);
    rig.frame();
    rig.dispatcher.keyEvent(XKB_KEY_Return, 0, 0, true);
    TEST_CHECK(rig.mores.size() == 2 && rig.mores.back().count == more.count);
    TEST_NEAR(rig.mores.back().sceneX, piece->x, 0.5F);
    TEST_NEAR(rig.mores.back().sceneY, piece->y, 0.5F);
  }

  void columnsReopenWhereTheUserWas() {
    Rig rig;
    rig.settle();
    TEST_CHECK(rig.lookahead.location(0) == rig.fixture.at("big")); // nothing remembered: the heaviest
    rig.open("mid/sub");
    rig.until([&rig] {
      const auto snapshot = rig.model.snapshot();
      return snapshot && snapshot->location == rig.fixture.at("mid/sub") && !rig.model.measuring();
    });
    rig.open("");
    rig.until([&rig] { return rig.lookahead.count() > 1 && rig.lookahead.location(0) == rig.fixture.at("mid"); });
    TEST_CHECK(rig.lookahead.location(1) == rig.fixture.at("mid/sub"));
    // the cursor wins over the trail
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("small"), .folder = true});
    rig.until([&rig] { return rig.lookahead.count() > 0 && rig.lookahead.location(0) == rig.fixture.at("small"); });
  }

  void enteringAFolderMovesTheCenter() {
    Rig rig;
    rig.settle();
    rig.open("big");
    rig.frame();
    TEST_CHECK(rig.view->center().moving() && rig.list->translateX() > 0 && rig.list->opacity() < 1);
    rig.finishTransitions();
    TEST_CHECK(!rig.view->center().moving() && rig.list->translateX() == 0 && rig.list->opacity() == 1);
    rig.open("");
    rig.frame();
    TEST_CHECK(rig.view->center().moving() && rig.list->translateX() < 0);
    rig.finishTransitions();
    TEST_CHECK(rig.list->translateX() == 0 && rig.list->opacity() == 1);
    // the instrument expands over the moving center: the motion ends at the next frame
    rig.open("big");
    rig.frame();
    TEST_CHECK(rig.view->center().moving());
    rig.view->shell().setState(wb::withExpanded(rig.view->shell().state(), true));
    rig.frame();
    rig.frame();
    TEST_CHECK(!shown(&rig.view->center()) && !rig.view->center().moving() && rig.list->opacity() == 1);
  }

  // A file preview taking column A in the frame that enters the folder leaves columns B and C in place.
  void previewAsTheFolderChangesLeavesTheColumns() {
    Rig rig;
    WorkbenchGeometry narrowStrips; // as wide as the fixture's three bands, so a second column ahead fits
    narrowStrips.strips = 107;
    rig.view->setGeometry(narrowStrips);
    rig.settle();
    (void)rig.view->setPreview(std::make_unique<::Node>()); // the page's preview slot
    rig.open("big");
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big/b.bin"), .folder = false});
    rig.frame();
    TEST_CHECK(rig.view->center().moving() && rig.lookahead.preview().has_value());
    rig.finishTransitions();
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big/deep"), .folder = true});
    rig.until([&rig] { return rig.lookahead.count() > 1 && !rig.lookahead.preview(); });
    rig.finishTransitions();
    for (std::size_t slot = 1; slot < 3; ++slot) {
      const auto* column = rig.view->center().column(slot);
      TEST_CHECK(column->opacity() == 1 && column->translateX() == 0);
    }
    TEST_CHECK(shown(rig.view->center().column(1)));
  }
} // namespace

int main() {
  middleClicksAskForNewTabs();
  stripKeyLeavesTheFocusOnTheStrips();
  mergedPieceListsWhatItHolds();
  columnsReopenWhereTheUserWas();
  enteringAFolderMovesTheCenter();
  previewAsTheFolderChangesLeavesTheColumns();
  std::println("workbench-view-links: all checks passed");
  return 0;
}
