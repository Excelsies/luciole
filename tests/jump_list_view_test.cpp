#include "jump_list_view_rig.h"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace luciole::test::jump_rig;
  using namespace luciole;

  void testRowsShowNamePathAndShare() {
    Rig rig;
    auto list = fixtureList();
    TEST_CHECK(list.rows.size() == 4);
    list.rows[2].share = 0.25;
    rig.show({.list = list});
    TEST_CHECK(rig.view->notice() == JumpNotice::None);
    TEST_CHECK((rig.names() == Strings{"x", "deep", "y", "z"}));
    Strings paths;
    for (const auto* row : rowsOf(*rig.view)) {
      paths.push_back(row->pathLabel().text());
      TEST_CHECK(row->pathLabel().fontSize() < row->nameLabel().fontSize());
    }
    TEST_CHECK((paths == Strings{"a/x", "a/x/deep", "a/y", "b/z"}));
    TEST_NEAR(rowAt(*rig.view, "a/y")->share(), 0.25, 1e-6);
    TEST_CHECK(!noticeShows(rig, "Nothing two levels down"));
    // each row shows its folder's value in the measure entries are sized by, in small text
    Strings values;
    for (const auto* row : rowsOf(*rig.view)) {
      values.push_back(row->valueLabel().text());
      TEST_CHECK(row->valueLabel().fontSize() < row->nameLabel().fontSize());
      TEST_CHECK(row->valueLabel().width() > 0);
    }
    TEST_CHECK((values == Strings{"400 bytes", "100 bytes", "300 bytes", "200 bytes"}));
  }

  // A middle click asks for a new tab and neither activates nor moves the cursor.
  void testMiddleClickAsksForANewTab() {
    Rig rig;
    rig.show({.list = fixtureList()});
    rig.view->setActivationMode(ActivationMode::SingleClick);
    rig.click(centerOf(rowAt(*rig.view, "a/y")), BTN_MIDDLE);
    TEST_CHECK((rig.middles == Strings{"a/y"}));
    TEST_CHECK(rig.activations.empty() && !rig.view->cursor());
    TEST_CHECK(rig.dispatcher.focusedArea() == nullptr);
    rig.click({.x = 100, .y = 300}, BTN_MIDDLE); // below the rows
    TEST_CHECK(rig.middles.size() == 1);
    rig.click(centerOf(rowAt(*rig.view, "b/z")));
    TEST_CHECK((rig.activations == Strings{"b/z"}) && rig.middles.size() == 1);
  }

  void testHoverAndLinkMarkTheirRow() {
    Rig rig;
    auto list = fixtureList();
    rig.show({.list = list});
    rig.move(centerOf(rowAt(*rig.view, "a/y")));
    TEST_CHECK((rig.hovers == std::vector<std::optional<std::string>>{"a/y"}));
    TEST_CHECK(rowAt(*rig.view, "a/y")->marks().hovered);
    TEST_CHECK(!rowAt(*rig.view, "a/x")->marks().hovered);
    rig.move(centerOf(rowAt(*rig.view, "b/z")));
    TEST_CHECK(rig.hovers.size() == 2 && rig.hovers[1] == "b/z");
    TEST_CHECK(!rowAt(*rig.view, "a/y")->marks().hovered);
    rig.move({.x = 100, .y = 300}); // below the rows
    TEST_CHECK(rig.hovers.size() == 3 && rig.hovers[2] == std::nullopt);
    rig.move({.x = 1500, .y = 1500});
    TEST_CHECK(rig.hovers.size() == 3);
    rig.move(centerOf(rowAt(*rig.view, "a/x")));
    rig.move({.x = 1500, .y = 1500});
    TEST_CHECK(rig.hovers.size() == 5 && rig.hovers[4] == std::nullopt);
    TEST_CHECK(!rowAt(*rig.view, "a/x")->marks().hovered);
    const auto linked = std::ranges::find(list.rows, std::string("a/x/deep"), &JumpRow::path)->id;
    rig.show({.list = list, .linked = linked});
    TEST_CHECK(rowAt(*rig.view, "a/x/deep")->marks().linked);
    for (const auto* path : {"a/x", "a/y", "b/z"}) {
      TEST_CHECK(!rowAt(*rig.view, path)->marks().linked);
    }
  }

  void testClicksFollowTheActivationSetting() {
    Rig rig;
    rig.show({.list = fixtureList()});
    rig.view->setActivationMode(ActivationMode::SingleClick);
    rig.click(centerOf(rowAt(*rig.view, "a/x/deep")));
    TEST_CHECK((rig.activations == Strings{"a/x/deep"}));
    TEST_CHECK(rig.view->cursor() == 1);
    rig.view->setActivationMode(ActivationMode::DoubleClick);
    const auto y = centerOf(rowAt(*rig.view, "a/y"));
    rig.click(y);
    TEST_CHECK(rig.activations.size() == 1);
    TEST_CHECK(rig.view->cursor() == 2);
    rig.click(y);
    TEST_CHECK((rig.activations == Strings{"a/x/deep", "a/y"}));
  }

  void testKeysMoveAndActivate() {
    Rig rig;
    rig.show({.list = fixtureList()});
    rig.dispatcher.setFocus(rig.view->focusArea());
    TEST_CHECK(!rig.view->cursor());
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.view->cursor() == 0);
    TEST_CHECK(rowAt(*rig.view, "a/x")->cursor());
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.view->cursor() == 1);
    TEST_CHECK(rowAt(*rig.view, "a/x/deep")->cursor());
    TEST_CHECK(!rowAt(*rig.view, "a/x")->cursor());
    rig.key(XKB_KEY_End);
    TEST_CHECK(rig.view->cursor() == 3);
    rig.key(XKB_KEY_Down); // stays on the last row
    TEST_CHECK(rig.view->cursor() == 3);
    rig.key(XKB_KEY_Page_Up);
    TEST_CHECK(rig.view->cursor() == 0);
    rig.key(XKB_KEY_Page_Down);
    TEST_CHECK(rig.view->cursor() == 3);
    rig.key(XKB_KEY_Home);
    rig.key(XKB_KEY_Up);
    TEST_CHECK(rig.view->cursor() == 0);
    rig.key(XKB_KEY_Return);
    TEST_CHECK((rig.activations == Strings{"a/x"}));
    rig.key(XKB_KEY_Down);
    rig.key(XKB_KEY_space);
    TEST_CHECK((rig.activations == Strings{"a/x", "a/x/deep"}));
    // A shorter list keeps the cursor on a row.
    rig.key(XKB_KEY_End);
    auto list = fixtureList();
    list.rows.resize(2);
    rig.show({.list = list});
    TEST_CHECK(rig.view->cursor() == 1);
  }

  // A list still being measured may rank its rows again; the cursor stays on its folder.
  void testCursorFollowsItsRowThroughARerank() {
    Rig rig;
    rig.show({.list = fixtureList()});
    rig.dispatcher.setFocus(rig.view->focusArea());
    rig.key(XKB_KEY_Down);
    rig.key(XKB_KEY_Down);
    TEST_CHECK(rig.view->cursor() == 1);
    auto list = fixtureList();
    list.partial = true;
    std::swap(list.rows[1], list.rows[3]);
    rig.show({.list = list});
    TEST_CHECK(rig.view->cursor() == 3);
    TEST_CHECK(rowAt(*rig.view, "a/x/deep")->cursor());
    TEST_CHECK(!rowAt(*rig.view, "b/z")->cursor());
    rig.key(XKB_KEY_Return);
    TEST_CHECK((rig.activations == Strings{"a/x/deep"}));
    // A row that is gone leaves the cursor at its place, within the rows.
    list.rows.erase(list.rows.begin() + 3);
    rig.show({.list = list});
    TEST_CHECK(rig.view->cursor() == 2);
    TEST_CHECK(rowAt(*rig.view, "a/y")->cursor());
  }

  // New rows under a resting pointer report the row now under it.
  void testHoverFollowsNewRowsUnderThePointer() {
    Rig rig;
    auto list = fixtureList();
    rig.show({.list = list});
    rig.move(centerOf(rowAt(*rig.view, "a/y")));
    TEST_CHECK((rig.hovers == std::vector<std::optional<std::string>>{"a/y"}));
    std::swap(list.rows[2], list.rows[3]);
    rig.show({.list = list});
    TEST_CHECK(rig.hovers.size() == 2 && rig.hovers[1] == "b/z");
    TEST_CHECK(rowAt(*rig.view, "b/z")->marks().hovered);
    TEST_CHECK(!rowAt(*rig.view, "a/y")->marks().hovered);
  }

  void testWheelMovesTheHover() {
    Rig rig;
    rig.show({.list = longList(30)});
    const auto at = centerOf(rowAt(*rig.view, "p/f02"));
    rig.move(at);
    TEST_CHECK((rig.hovers == std::vector<std::optional<std::string>>{"p/f02"}));
    for (int notch = 0; notch < 3; ++notch) {
      TEST_CHECK(rig.dispatcher.pointerAxis(at.x, at.y, 0, 0, 20, 1, 120, 0));
      rig.layout();
    }
    rig.layout();
    const JumpRowTile* under = nullptr;
    for (const auto* row : rowsOf(*rig.view)) {
      const auto center = centerOf(row);
      if (std::abs(center.y - at.y) < row->height() / 2) {
        under = row;
      }
    }
    TEST_CHECK(under != nullptr && under->row().path != "p/f02");
    TEST_CHECK(!rig.hovers.empty() && rig.hovers.back() == under->row().path);
    for (const auto* row : rowsOf(*rig.view)) {
      TEST_CHECK(row->marks().hovered == (row == under));
    }
  }

  void testNoticesAndSerial() {
    Rig rig;
    rig.show({.list = {}});
    TEST_CHECK(rig.view->notice() == JumpNotice::Empty);
    TEST_CHECK(noticeShows(rig, "Nothing two levels down"));
    TEST_CHECK(rowsOf(*rig.view).empty());
    // Folders a walk cut short are not running: with none two levels down the list is empty, not measuring, and a
    // row for one is not dimmed.
    const auto cutShort = [](bool withDeep) {
      auto tree = luciole::test::workbenchTree(
          {{.parent = -1, .name = "r", .folder = true, .items = 1},
           {.parent = 0, .name = "a", .folder = true, .items = 1},
           {.parent = 1, .name = "deep", .folder = true}},
          0
      );
      if (!withDeep) {
        tree.nodes.pop_back();
        tree.children.pop_back();
        tree.nodes[1].childCount = 0;
      }
      for (auto& node : tree.nodes) {
        node.lowerBound = true;
      }
      return tree;
    };
    const auto ranked = luciole::workbench::jumpAhead(cutShort(true), LayoutSettings{});
    TEST_CHECK(ranked.rows.size() == 1 && !ranked.partial && !ranked.rows[0].partial);
    rig.show({.list = ranked});
    TEST_CHECK(!rowAt(*rig.view, "a/deep")->row().partial);
    const auto none = luciole::workbench::jumpAhead(cutShort(false), LayoutSettings{});
    TEST_CHECK(none.rows.empty() && !none.partial);
    rig.show({.list = none});
    TEST_CHECK(rig.view->notice() == JumpNotice::Empty);
    rig.show({.list = {.rows = {}, .partial = true, .cut = false}});
    TEST_CHECK(rig.view->notice() == JumpNotice::Measuring);
    TEST_CHECK(noticeShows(rig, "Measuring…"));
    TEST_CHECK(!noticeShows(rig, "Nothing two levels down"));
    rig.show({.list = fixtureList()});
    TEST_CHECK(rig.view->notice() == JumpNotice::None);
    TEST_CHECK(!noticeShows(rig, "Measuring…"));
    // The same serial changes nothing.
    rig.view->setContent({.list = {}}, rig.contentSerial);
    rig.layout();
    TEST_CHECK(rig.view->notice() == JumpNotice::None);
    TEST_CHECK(rowsOf(*rig.view).size() == 4);
    // A row still being measured shows its new share once its serial moves.
    auto list = fixtureList();
    list.rows[3].share = 0.75;
    list.rows[3].partial = true;
    rig.show({.list = list});
    TEST_NEAR(rowAt(*rig.view, "b/z")->share(), 0.75, 1e-6);
    TEST_CHECK(rowAt(*rig.view, "b/z")->row().partial);
  }
} // namespace

int main() {
  testRowsShowNamePathAndShare();
  testMiddleClickAsksForANewTab();
  testHoverAndLinkMarkTheirRow();
  testClicksFollowTheActivationSetting();
  testKeysMoveAndActivate();
  testCursorFollowsItsRowThroughARerank();
  testHoverFollowsNewRowsUnderThePointer();
  testWheelMovesTheHover();
  testNoticesAndSerial();
  std::println("jump list view checks passed");
}
