#include "test_check.h"
#include "workbench/iris_hit.h"
#include "workbench/iris_keys.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace {
  using namespace luciole::test::iris;

  using Stop = std::optional<std::size_t>;

  // 60 files on a full dial: both columns hold rows, and "+N more" is the last arc, so it tops the left column.
  struct Files {
    FileFolder folder = fileFolder("f");
    Scene scene = buildScene(folder.tree, fullInput()).value();
  };

  std::size_t moreSector(const Scene& scene) {
    const auto found =
        std::ranges::find_if(scene.main.sectors, [](const Sector& s) { return s.kind == SectorKind::More; });
    TEST_CHECK(found != scene.main.sectors.end());
    return static_cast<std::size_t>(found - scene.main.sectors.begin());
  }

  void theCursorsRowIsFound() {
    const Files files;
    const auto& scene = files.scene;
    const auto& first = scene.rows.right.front();
    const auto node = scene.main.sectors[first.sector].node;
    TEST_CHECK(cursorRow(scene, node, CursorStop::Marks) == Stop(first.sector));
    TEST_CHECK(cursorRow(scene, node, CursorStop::More) == Stop(moreSector(scene)));
    TEST_CHECK(!cursorRow(scene, kNoNode, CursorStop::Marks));
    // An entry inside "+N more" has no row of its own.
    const auto merged = scene.main.sectors[moreSector(scene)].merged.front();
    TEST_CHECK(!cursorRow(scene, merged, CursorStop::Marks));
  }

  void walksStayInTheirColumn() {
    const Files files;
    const auto& scene = files.scene;
    const auto& right = scene.rows.right;
    TEST_CHECK(right.size() > 4 && !scene.rows.left.empty());
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::Down) == Stop(right[0].sector));
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::PageUp) == Stop(right[0].sector));
    TEST_CHECK(keyTarget(scene, right[0].sector, KeyMove::Down) == Stop(right[1].sector));
    TEST_CHECK(keyTarget(scene, right[0].sector, KeyMove::Up) == Stop(right[0].sector));
    TEST_CHECK(keyTarget(scene, right.back().sector, KeyMove::Down) == Stop(right.back().sector));
    const auto page = std::max<std::size_t>(1, scene.dial.rowsFit / 2);
    const auto paged = std::min(right.size() - 1, page);
    TEST_CHECK(keyTarget(scene, right[0].sector, KeyMove::PageDown) == Stop(right[paged].sector));
    TEST_CHECK(keyTarget(scene, right[paged].sector, KeyMove::PageUp) == Stop(right[0].sector));
  }

  // "+N more" is a stop like any row: End reaches it, Down and Up walk onto it and off it.
  void moreIsAStop() {
    const Files files;
    const auto& scene = files.scene;
    const auto& left = scene.rows.left;
    const auto more = moreSector(scene);
    TEST_CHECK(left.front().sector == more);
    TEST_CHECK(keyTarget(scene, scene.rows.right[2].sector, KeyMove::Home) == Stop(scene.rows.right.front().sector));
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::End) == Stop(more));
    TEST_CHECK(keyTarget(scene, left[1].sector, KeyMove::Up) == Stop(more));
    TEST_CHECK(keyTarget(scene, more, KeyMove::Down) == Stop(left[1].sector));
  }

  void acrossGoesToTheNearestRow() {
    const Files files;
    const auto& scene = files.scene;
    const auto& right = scene.rows.right;
    const auto& left = scene.rows.left;
    const auto& from = right[right.size() / 2];
    const auto target = keyTarget(scene, from.sector, KeyMove::Left);
    TEST_CHECK(target.has_value());
    const auto landed = std::ranges::find_if(left, [&target](const LabelRow& row) { return row.sector == *target; });
    TEST_CHECK(landed != left.end());
    for (const auto& row : left) {
      TEST_CHECK(std::abs(landed->y - from.y) <= std::abs(row.y - from.y));
    }
    TEST_CHECK(!keyTarget(scene, from.sector, KeyMove::Right));
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::Left) == Stop(left.front().sector));
    TEST_CHECK(keyTarget(scene, target, KeyMove::Right).has_value());
  }

  void nothingToWalk() {
    const Files files;
    TEST_CHECK(!keyTarget(files.scene, 9999, KeyMove::Down));
    auto empty = luciole::test::referenceTree();
    empty.root = 4; // c, an empty folder
    const auto scene = buildScene(empty, fullInput()).value();
    for (const auto move :
         {KeyMove::Down, KeyMove::Up, KeyMove::PageDown, KeyMove::PageUp, KeyMove::Home, KeyMove::End, KeyMove::Left,
          KeyMove::Right}) {
      TEST_CHECK(!keyTarget(scene, std::nullopt, move));
    }
    TEST_CHECK(!cursorRow(scene, kNoNode, CursorStop::More));
  }

  // A compact dial without a legend shows no rows, and its rows still serve the keyboard.
  void hiddenRowsServeTheKeyboard() {
    const auto folder = fileFolder("f");
    const auto scene = buildScene(folder.tree, hostInput(340, 534, true)).value();
    TEST_CHECK(!scene.dial.legend && !scene.rows.right.front().shown);
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::Down) == Stop(scene.rows.right.front().sector));
    TEST_CHECK(keyTarget(scene, std::nullopt, KeyMove::End) == Stop(moreSector(scene)));
  }

  // The list of "+N more" opens from its row, or from the hub when no row shows.
  void moreListsOpenFromTheirRow() {
    const Files files;
    const auto& full = files.scene;
    const auto anchor = moreAnchor(full);
    TEST_CHECK((hitTest(full, anchor) == Hit{.kind = HitKind::Row, .ring = 0, .sector = moreSector(full)}));
    TEST_NEAR(anchor.y, full.rows.left.front().y, 1e-9);
    const auto& box = full.rows.left.front().box;
    TEST_NEAR(anchor.x, box.x + box.width / 2, 1e-9);
    const auto hidden = buildScene(files.folder.tree, hostInput(340, 534, true)).value();
    TEST_CHECK(moreAnchor(hidden) == hidden.dial.center);
    TEST_CHECK((hitTest(hidden, moreAnchor(hidden)) == Hit{.kind = HitKind::Hub, .ring = 0, .sector = 0}));
  }
} // namespace

int main() {
  theCursorsRowIsFound();
  walksStayInTheirColumn();
  moreIsAStop();
  acrossGoesToTheNearestRow();
  nothingToWalk();
  hiddenRowsServeTheKeyboard();
  moreListsOpenFromTheirRow();
  return 0;
}
