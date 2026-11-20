#include "test_check.h"
#include "ui/workbench/kept_snapshots.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <cstdint>
#include <ctime>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace {
  namespace wb = luciole::workbench;
  using luciole::KeptSnapshots;
  using luciole::Location;
  using luciole::PartSnapshot;
  using luciole::PlacedSnapshot;
  using luciole::WorkbenchSnapshot;
  using Strings = std::vector<std::string>;

  std::shared_ptr<const WorkbenchSnapshot> snapshotOf(std::uint64_t serial, wb::Tree tree) {
    return std::make_shared<const WorkbenchSnapshot>(WorkbenchSnapshot{
        .serial = serial,
        .location = Location::fromPath("/r"),
        .settings = {},
        .tree = std::move(tree),
        .jump = {},
        .jumpLocations = {},
        .shares = {},
        .total = 0,
        .error = std::nullopt,
    });
  }

  // /r with: a/ 900 (x/ 500, y.txt 100), b.txt 600, .c/ 300 (hidden), d.txt 200, e/ 200, f.txt 50, and 40 small
  // files folded into one remainder node. Ids: 0 r, 1 a, 2 b.txt, 3 .c, 4 d.txt, 5 e, 6 f.txt, 7 x, 8 y.txt, 9 the
  // remainder.
  std::shared_ptr<const WorkbenchSnapshot> snapshot() {
    auto tree = luciole::test::workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .bytes = 2250, .items = 6},
            {.parent = 0, .name = "a", .folder = true, .bytes = 900, .items = 2},
            {.parent = 0, .name = "b.txt", .bytes = 600},
            {.parent = 0, .name = ".c", .folder = true, .bytes = 300},
            {.parent = 0, .name = "d.txt", .bytes = 200},
            {.parent = 0, .name = "e", .folder = true, .bytes = 200},
            {.parent = 0, .name = "f.txt", .bytes = 50},
            {.parent = 1, .name = "x", .folder = true, .bytes = 500},
            {.parent = 1, .name = "y.txt", .bytes = 100},
            {.parent = 0, .name = "", .bytes = 30, .items = 40},
        },
        6000
    );
    return snapshotOf(7, std::move(tree));
  }

  PartSnapshot partOf(const std::shared_ptr<const WorkbenchSnapshot>& placed) {
    return {.tree = {placed, &placed->tree}, .serial = placed->serial};
  }

  std::vector<wb::Shape> drawn(std::initializer_list<wb::NodeId> nodes) {
    std::vector<wb::Shape> shapes;
    for (const auto node : nodes) {
      wb::Shape shape;
      shape.id = {.node = node, .role = 0};
      shapes.push_back(shape);
    }
    return shapes;
  }

  Strings names(const luciole::MergedEntries& merged) {
    Strings out;
    for (const auto& entry : merged.listed) {
      out.push_back(entry.name);
    }
    return out;
  }

  // The entries no shape stands for, heaviest first, with their places and values.
  void listsWhatTheSceneDoesNotDraw() {
    const auto made = snapshot();
    KeptSnapshots kept;
    kept.add(luciole::placeSnapshot(made));
    const wb::LayoutSettings settings;
    const auto merged = kept.mergedIn(partOf(made), 0, drawn({1, 2}), settings, false, 10);
    TEST_CHECK((names(merged) == Strings{"d.txt", "e", "f.txt"}));
    TEST_CHECK(merged.count == 43); // the folded files count, unlisted
    TEST_CHECK(kept.mergedIn(partOf(made), 0, drawn({1, 2, 9}), settings, false, 10).count == 3);
    TEST_CHECK(merged.listed[0].entry.location == Location::fromPath("/r/d.txt") && !merged.listed[0].entry.folder);
    TEST_CHECK(merged.listed[1].entry.location == Location::fromPath("/r/e") && merged.listed[1].entry.folder);
    TEST_CHECK(merged.listed[0].value == "200 bytes" && merged.listed[2].value == "50 bytes");
    // equal weights keep the name order: d.txt before e
    TEST_CHECK(merged.listed[0].name < merged.listed[1].name);
    // with hidden entries shown, .c joins in its place by weight
    auto shown = settings;
    shown.showHidden = true;
    TEST_CHECK(
        (names(kept.mergedIn(partOf(made), 0, drawn({1, 2}), shown, false, 10)) == Strings{".c", "d.txt", "e", "f.txt"})
    );
    // folders only, for ribbons that draw no files
    const auto folders = kept.mergedIn(partOf(made), 0, drawn({1}), settings, true, 10);
    TEST_CHECK((names(folders) == Strings{"e"}) && folders.count == 1);
    // a deeper folder
    TEST_CHECK((names(kept.mergedIn(partOf(made), 1, drawn({}), settings, false, 10)) == Strings{"x", "y.txt"}));
  }

  void limitCountsEverything() {
    const auto made = snapshot();
    KeptSnapshots kept;
    kept.add(luciole::placeSnapshot(made));
    const auto merged = kept.mergedIn(partOf(made), 0, drawn({}), wb::LayoutSettings{}, false, 2);
    TEST_CHECK((names(merged) == Strings{"a", "b.txt"}) && merged.count == 45);
    TEST_CHECK(kept.mergedIn(partOf(made), 0, drawn({}), wb::LayoutSettings{}, false, 0).listed.empty());
  }

  void unknownSnapshotsAndNodesListNothing() {
    const auto made = snapshot();
    KeptSnapshots kept;
    const auto none = kept.mergedIn(partOf(made), 0, drawn({}), wb::LayoutSettings{}, false, 10);
    TEST_CHECK(none.listed.empty() && none.count == 0); // not kept
    kept.add(luciole::placeSnapshot(made));
    for (const wb::NodeId node : {wb::NodeId{2}, wb::NodeId{99}, wb::kNoNode}) { // a file, out of range, none
      const auto merged = kept.mergedIn(partOf(made), node, drawn({}), wb::LayoutSettings{}, false, 10);
      TEST_CHECK(merged.listed.empty() && merged.count == 0);
    }
    TEST_CHECK(kept.mergedIn({}, 0, drawn({}), wb::LayoutSettings{}, false, 10).count == 0);
  }

  // A folder of 20,000 entries lists its heaviest hundred in well under the time of a frame.
  void largeFolderIsQuick() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .bytes = 1}};
    std::vector<std::string> labels;
    labels.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
      labels.push_back(std::format("f{:05}", i));
    }
    for (int i = 0; i < 20000; ++i) {
      rows.push_back({.parent = 0, .name = labels[static_cast<std::size_t>(i)], .bytes = static_cast<double>(i % 997)});
    }
    const auto made = snapshotOf(1, luciole::test::workbenchTree(rows, 6000));
    KeptSnapshots kept;
    kept.add(luciole::placeSnapshot(made));
    const auto shapes = drawn({1, 2, 3});
    const auto start = std::clock();
    const auto merged = kept.mergedIn(partOf(made), 0, shapes, wb::LayoutSettings{}, false, 100);
    const double ms = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    std::println(
        "20000 entries: {} listed of {} in {:.2f} ms of processor time", merged.listed.size(), merged.count, ms
    );
    TEST_CHECK(merged.listed.size() == 100 && merged.count == 19997);
    TEST_CHECK(merged.listed.front().name == "f00996" && merged.listed.front().value == "996 bytes");
    TEST_CHECK(ms < 50);
  }

  // Where a merged piece is drawn: the middle of the box around its hittable shapes, whatever their kinds.
  void anchorIsTheMiddleOfTheMergedPiece() {
    const auto more = wb::shapeRole(wb::SceneRole::More);
    std::vector<wb::Shape> shapes{
        wb::rectShape({.node = 0, .role = more}, 10, 20, 40, 10),
        wb::sectorShape({.node = 0, .role = more}, {.x = 50, .y = 30}, 0, 10, 90, 180), // x 50 to 60, y 30 to 40
        wb::rectShape({.node = 0, .role = wb::shapeRole(wb::SceneRole::Piece)}, 500, 500, 10, 10),
        wb::rectShape({.node = 0, .role = more}, 900, 900, 0, 10), // no size
        wb::rectShape({.node = 1, .role = more}, 700, 700, 10, 10),
        wb::sectorShape({.node = 2, .role = more}, {.x = 0, .y = 0}, 0, 10, -45, 45),
        wb::circleShape({.node = 3, .role = more}, {.x = 5, .y = 6}, 4),
    };
    auto covered = wb::rectShape({.node = 0, .role = more}, 800, 800, 10, 10);
    covered.hittable = false;
    shapes.push_back(covered);
    const auto at = [&shapes](wb::NodeId node, float x, float y) {
      const auto anchor = luciole::mergedAnchor(shapes, node);
      TEST_CHECK(anchor.has_value());
      TEST_NEAR(anchor->x, x, 0.001F);
      TEST_NEAR(anchor->y, y, 0.001F);
    };
    at(0, 35, 30);
    at(1, 705, 705);
    at(2, 0, -5); // the outer edge reaches 12 o'clock between the sector's ends
    at(3, 5, 6);
    TEST_CHECK(!luciole::mergedAnchor(shapes, 4) && !luciole::mergedAnchor({}, 0));
  }
} // namespace

int main() {
  listsWhatTheSceneDoesNotDraw();
  limitCountsEverything();
  unknownSnapshotsAndNodesListNothing();
  largeFolderIsQuick();
  anchorIsTheMiddleOfTheMergedPiece();
  std::println("kept snapshots checks passed");
}
