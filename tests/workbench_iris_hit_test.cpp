#include "test_check.h"
#include "workbench/iris_hit.h"
#include "workbench/iris_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench_iris_fixture.h"

#include <print>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::workbench::iris;
  using namespace luciole::test::iris;
  using ring::Point;

  void testHitTestFindsRowsHubArcsAndRings() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    const auto at = [&scene](double radius, double degrees) {
      return hitTest(scene, ring::polar(scene.dial.center, radius, degrees));
    };
    TEST_CHECK((hitTest(scene, Point{700, 409}) == Hit{.kind = HitKind::Hub, .ring = 0, .sector = 0}));
    TEST_CHECK((at(180, 90) == Hit{.kind = HitKind::Arc, .ring = 0, .sector = 0}));
    TEST_CHECK((at(180, 300) == Hit{.kind = HitKind::Arc, .ring = 0, .sector = 1}));
    TEST_CHECK((at(255, 0) == Hit{.kind = HitKind::Context, .ring = 0, .sector = 0}));
    TEST_CHECK((at(242.6, 180) == Hit{.kind = HitKind::Context, .ring = 0, .sector = 1}));
    TEST_CHECK((at(275, 120) == Hit{.kind = HitKind::Context, .ring = 1, .sector = 1}));
    TEST_CHECK(!at(242.2, 0).has_value());
    TEST_CHECK(!at(117, 0).has_value());
    TEST_CHECK(!at(400, 0).has_value());
    TEST_CHECK((hitTest(scene, Point{1100, 560}) == Hit{.kind = HitKind::Row, .ring = 0, .sector = 0}));
    TEST_CHECK((hitTest(scene, Point{20, 258}) == Hit{.kind = HitKind::Row, .ring = 0, .sector = 1}));
    TEST_CHECK(!hitTest(scene, Point{5, 5}).has_value());
    TEST_CHECK(!hitTest(scene, Point{kNan, 5}).has_value());
    const auto folder = fileFolder("file-");
    const auto compact = buildScene(folder.tree, hostInput(340, 380, true)).value();
    TEST_CHECK(!hitTest(compact, Point{0, 0}).has_value());
  }

  // Ids of folderX: 0 r, 1 a, 5 x, 7 deep, 9 g.c. A chip names its folder by its place among the hub's chips.
  void testChipsAndStackRowsNameTheirFolder() {
    const auto tree = folderX();
    auto input = fullInput();
    input.trail = {7, kNoNode, 9};
    const auto scene = buildScene(tree, input).value();
    TEST_CHECK(scene.hub.chips.size() == 2);
    TEST_CHECK(chipId(0).node == kNoNode && chipId(0) != chipId(1));
    TEST_CHECK(sceneRole(chipId(1).role) == SceneRole::Chip);
    TEST_CHECK(folderOf(scene, chipId(0)) == 7);
    TEST_CHECK(folderOf(scene, chipId(1)) == 9);
    TEST_CHECK(folderOf(scene, chipId(2)) == kNoNode);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 7, .role = shapeRole(SceneRole::Chip)}) == kNoNode);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 1, .role = shapeRole(SceneRole::Stack)}) == 1);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 0, .role = shapeRole(SceneRole::Stack)}) == 0);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 7, .role = shapeRole(SceneRole::Stack)}) == kNoNode);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 7, .role = shapeRole(SceneRole::Piece)}) == kNoNode);
    TEST_CHECK(folderOf(scene, ShapeId{.node = 5, .role = shapeRole(SceneRole::Hub)}) == kNoNode);
  }
} // namespace

int main() {
  testHitTestFindsRowsHubArcsAndRings();
  testChipsAndStackRowsNameTheirFolder();
  std::println("workbench iris hit checks passed");
  return 0;
}
