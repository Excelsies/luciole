#include "cpu_budget.h"
#include "test_check.h"
#include "workbench/atlas_camera.h"
#include "workbench/atlas_navigation.h"
#include "workbench/atlas_scene.h"
#include "workbench/pane_layout.h"
#include "workbench_atlas_fixture.h"
#include "workbench_fixture.h"

#include <ctime>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::atlas;
  using luciole::test::workbenchTree;

  void testZoomAboutKeepsThePointFixed() {
    checkCamera(zoomAbout({.cx = 0.5, .cy = -0.25, .scale = 200}, 1.6, 40, -30), 0.575, -0.30625, 320);
    const Camera camera{.cx = 0.5, .cy = -0.25, .scale = 200};
    for (const double factor : {0.0, -2.0, kNan, kInfinity}) {
      TEST_CHECK(zoomAbout(camera, factor, 40, -30) == camera);
    }
    const Camera flat{.cx = 0, .cy = 0, .scale = 0};
    TEST_CHECK(zoomAbout(flat, 2, 1, 1) == flat);
    // An offset that is not finite reads as the view center.
    checkCamera(zoomAbout(camera, 1.6, kNan, -30), 0.5, -0.30625, 320);
    checkCamera(zoomAbout(camera, 1.6, 40, kInfinity), 0.575, -0.25, 320);
  }

  void testConvert() {
    checkCamera(convert({.cx = 0.1, .cy = 0.2, .scale = 300}, {.ax = 0.3, .ay = -0.4, .k = 0.25}), 0.325, -0.35, 1200);
  }

  void testInterpolate() {
    const Camera from{.cx = 0, .cy = 0, .scale = 100};
    const Camera to{.cx = 1, .cy = -2, .scale = 400};
    checkCamera(interpolate(from, to, 0.3), 0.45366139281807055, -0.90732278563614110, 151.57165665103980);
    checkCamera(interpolate(from, {.cx = 1, .cy = -2, .scale = 100.000001}, 0.3), 0.3, -0.6, 100.0000003);
    TEST_CHECK(interpolate(from, to, 0) == from);
    TEST_CHECK(interpolate(from, to, -1) == from);
    TEST_CHECK(interpolate(from, to, 1) == to);
    TEST_CHECK(interpolate(from, to, 7) == to);
    TEST_CHECK(interpolate(from, to, kNan) == from);
    TEST_CHECK(interpolate({.cx = 0, .cy = 0, .scale = 0}, to, 0.5) == to);
    const Camera broken{.cx = 1, .cy = 1, .scale = -3};
    TEST_CHECK(interpolate(from, broken, 0.5) == broken);
  }

  void testNearest() {
    const std::vector<Point> points{{0, 0}, {1, 0.1}, {2, 0}, {0.5, 2}, {-1, 0}};
    TEST_CHECK(nearest(points, 0, 1, 0) == 1U);
    TEST_CHECK(nearest(points, 0, 0, 1) == 3U);
    TEST_CHECK(!nearest(points, 0, 0, -1));
    TEST_CHECK(nearest(points, 0, -1, 0) == 4U);
    const std::vector<Point> steep{{0, 0}, {0.2, 1}, {3, 0.5}};
    TEST_CHECK(nearest(steep, 0, 1, 0) == 2U);
    // Both lie in the cone; sideways distance costs 2.2 times distance ahead.
    const std::vector<Point> weighted{{0, 0}, {1, 0.5}, {1.8, 0}};
    TEST_CHECK(nearest(weighted, 0, 1, 0) == 2U);
    const std::vector<Point> outsideCone{{0, 0}, {0.1, 1}};
    TEST_CHECK(nearest(outsideCone, 0, 1, 0) == 1U);
    const std::vector<Point> behind{{0, 0}, {-1, 0}, {-2, 1}};
    TEST_CHECK(!nearest(behind, 0, 1, 0));
    TEST_CHECK(!nearest(points, 9, 1, 0));
    TEST_CHECK(!nearest({}, 0, 1, 0));
  }

  void testDistanceAndFrames() {
    World world;
    TEST_CHECK(treeDistance(world.tree, kSub, kMusic) == 3);
    TEST_CHECK(treeDistance(world.tree, kFirstS, kEtc) == 5);
    TEST_CHECK(treeDistance(world.tree, kDocs, kDocs) == 0);
    TEST_CHECK(treeDistance(world.tree, kDocs, 9999) == 0);
    const auto up = similarity(world.source, kDocs, kTop);
    TEST_CHECK(up.has_value());
    TEST_NEAR(up->ax, -0.56967493649460876, kExact);
    TEST_NEAR(up->ay, 0.093656644683086973, kExact);
    TEST_NEAR(up->k, 0.27423066877135094, kExact);
    const auto down = similarity(world.source, kTop, kDocs);
    TEST_NEAR(down->ax, 2.0773567706593545, kExact);
    TEST_NEAR(down->ay, -0.34152505663462596, kExact);
    TEST_NEAR(down->k, 3.6465651507190970, kExact);
    const auto across = similarity(world.source, kSub, kMusic);
    TEST_NEAR(across->ax, -2.1498875809008484, kExact);
    TEST_NEAR(across->ay, 0.059781341637314786, kExact);
    TEST_NEAR(across->k, 0.15359529845103154, kExact);
    TEST_CHECK(!similarity(world.source, kSub, 9999));
    const auto moved = convertCamera(world.source, kSub, kHome, {.cx = 0.1, .cy = -0.2, .scale = 500});
    TEST_CHECK(moved.has_value());
    checkCamera(*moved, -0.38868705055515185, 0.071948293736960711, 7918.0233546045820);
    TEST_CHECK(!convertCamera(world.source, kFirstS, kEtc, {.cx = 0, .cy = 0, .scale = 100}));
    TEST_CHECK(treeDistance(world.tree, kMusic, kSub) == 3);
  }

  // A single chain of `count` folders, each the only child of the one before.
  Tree chainTree(NodeId count) {
    Tree tree;
    tree.nodes.reserve(count);
    for (NodeId id = 0; id < count; ++id) {
      tree.nodes.push_back(
          {.name = "chain",
           .parent = id == 0 ? kNoNode : id - 1,
           .firstChild = id,
           .childCount = id + 1 < count ? 1U : 0U,
           .bytes = 1,
           .folder = true}
      );
      if (id + 1 < count) {
        tree.children.push_back(id + 1);
      }
    }
    tree.root = 0;
    return tree;
  }

  // Frames between neighbors deep in a long chain take time linear in the depth.
  void testDeepFramesAreQuick() {
    constexpr NodeId kDepth = 40000;
    const auto tree = chainTree(kDepth);
    LayoutSettings settings;
    PackingCache packings;
    const AtlasSource source{.tree = tree, .settings = settings, .packings = packings};
    const std::clock_t start = std::clock();
    const auto up = similarity(source, kDepth - 1, kDepth - 2);
    const double elapsed = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    std::println("  similarity at depth {} {:.2f} ms", kDepth, elapsed);
    TEST_CHECK(up.has_value());
    TEST_NEAR(up->ax, 0.0, kExact);
    TEST_NEAR(up->ay, 0.06, kExact);
    TEST_NEAR(up->k, 0.9, kExact);
    TEST_CHECK(::test::withinBudget(elapsed, 100));
  }

  // A folder whose weights cannot be packed has no frames for its children: no similarity, no conversion,
  // no promotion, and its children draw nothing.
  void testUnpackableFolderHasNoFrames() {
    const auto tree = workbenchTree(
        {
            {.parent = -1, .name = "root", .folder = true},
            {.parent = 0, .name = "huge", .folder = true, .bytes = 1e30},
            {.parent = 0, .name = "a", .folder = true},
            {.parent = 0, .name = "b"},
        },
        0
    );
    LayoutSettings settings;
    settings.emphasis = 1;
    PackingCache packings;
    const AtlasSource source{.tree = tree, .settings = settings, .packings = packings};
    TEST_CHECK(!similarity(source, 2, 0));
    TEST_CHECK(!similarity(source, 0, 2));
    TEST_CHECK(!similarity(source, 2, 3));
    TEST_CHECK(!convertCamera(source, 2, 0, {.cx = 0, .cy = 0, .scale = 100}));
    const auto state = settle(source, 2, {.cx = 0, .cy = 0, .scale = 10}, {600, 600}, false);
    TEST_CHECK(state.cwd == 2 && state.steps.empty());
    const auto scene = buildScene(source, 0, restCamera({600, 600}), {600, 600});
    TEST_CHECK(scene.items.size() == 1);
    TEST_CHECK(!scene.items[0].kids && !scene.items[0].empty);
    TEST_CHECK(buildScene(source, 2, restCamera({600, 600}), {600, 600}).items.size() == 1);
  }

  // The pane layout and the Atlas share one view size type, so a laid-out view feeds the Atlas as is.
  void testPaneLayoutViewFeedsTheAtlas() {
    const PaneLayout layout = layoutPanes(PaneState{}, {.width = 600, .height = 600}, PartLimits{}, RibbonKind::Flame);
    TEST_NEAR(restScale(layout.view), 286.0, 0.0);
  }

  // The smaller side sets the rest view and the thresholds, so a tall narrow pane behaves like a wide one.
  void testRestScaleFollowsTheSmallerSide() {
    TEST_NEAR(referenceLength({600, 600}), 600.0, 0.0);
    TEST_NEAR(restScale({600, 600}), 286.0, 0.0);
    TEST_NEAR(restScale({340, 600}), 156.0, 0.0);
    TEST_NEAR(restScale({600, 340}), 156.0, 0.0);
    TEST_NEAR(referenceLength({340, 600}), 340.0, 0.0);
    TEST_NEAR(restScale({260, 700}), 116.0, 0.0);
    TEST_NEAR(referenceLength({260, 700}), 260.0, 0.0);
    TEST_NEAR(restScale({100, 900}), 40.0, 0.0);
    TEST_NEAR(referenceLength({0, 0}), 1.0, 0.0);
    TEST_NEAR(restScale({0, 0}), 40.0, 0.0);
    TEST_NEAR(referenceLength({kNan, 500}), 1.0, 0.0);
    TEST_NEAR(restScale({kNan, 500}), 40.0, 0.0);
    TEST_NEAR(referenceLength({kInfinity, 500}), 500.0, 0.0);
    TEST_NEAR(referenceLength({kInfinity, kInfinity}), 1.0, 0.0);
    TEST_NEAR(restScale({kInfinity, kInfinity}), 40.0, 0.0);
    TEST_NEAR(referenceLength({-5, 500}), 1.0, 0.0);
    TEST_CHECK((restCamera({600, 600}) == Camera{.cx = 0, .cy = 0, .scale = 286}));
  }

} // namespace

int main() {
  testZoomAboutKeepsThePointFixed();
  testConvert();
  testInterpolate();
  testNearest();
  testDistanceAndFrames();
  testDeepFramesAreQuick();
  testUnpackableFolderHasNoFrames();
  testPaneLayoutViewFeedsTheAtlas();
  testRestScaleFollowsTheSmallerSide();
  std::println("workbench atlas camera checks passed");
  return 0;
}
