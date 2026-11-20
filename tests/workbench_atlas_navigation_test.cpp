#include "test_check.h"
#include "workbench/atlas_navigation.h"
#include "workbench_atlas_fixture.h"
#include "workbench_fixture.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::atlas;
  using luciole::test::WorkbenchRow;
  using luciole::test::workbenchTree;

  std::optional<PackedItem> placed(World& world, NodeId folder, NodeId child) {
    return world.packings.packing(world.tree, folder, world.settings, std::vector<NodeId>{kTop, kHome, kDocs})
        ->placement(child);
  }

  void checkState(
      const CameraState& state, NodeId cwd, double cx, double cy, double scale, const std::vector<NodeId>& steps
  ) {
    TEST_CHECK(state.cwd == cwd);
    checkCamera(state.camera, cx, cy, scale);
    TEST_CHECK(state.steps == steps);
  }

  void testProbe() {
    World world;
    const auto levels = probe(world.source, kDocs, restCamera({600, 600}));
    TEST_CHECK(levels.size() == 2);
    TEST_CHECK(levels[0].node == kSub && levels[1].node == kFirstS + 3);
    TEST_NEAR(levels[0].r, 36.942189307222563, kExact);
    TEST_NEAR(levels[1].r, 10.670859673008104, kExact);
    TEST_CHECK(probe(world.source, kDocs, {.cx = 5, .cy = 5, .scale = 286}).empty());
    TEST_CHECK(probe(world.source, kNotes, restCamera({600, 600})).empty());
    TEST_CHECK(probe(world.source, 9999, restCamera({600, 600})).empty());
    TEST_CHECK(probe(world.source, kDocs, {.cx = kNan, .cy = 0, .scale = 286}).empty());
  }

  // Zoom in on a folder until it becomes the current one, then out until the top, which stops at 60% of rest.
  void testDiveAndSurface() {
    World world;
    const ViewSize view{600, 600};
    const auto sub = placed(world, kDocs, kSub);
    CameraState state{.cwd = kDocs, .camera = restCamera(view), .steps = {}};
    const auto zoomIn = [&] {
      const double px = 300 + (sub->x - state.camera.cx) * state.camera.scale;
      const double py = 300 + (sub->y - state.camera.cy) * state.camera.scale;
      state = zoomAt(world.source, state.cwd, state.camera, view, 1.25, px, py);
    };
    for (int step = 0; step < 6; ++step) {
      zoomIn();
    }
    checkState(state, kDocs, 0.024336788682351271, 0.037095048633494034, 1091.0034179687500, {});
    zoomIn();
    checkState(state, kSub, -0.053550677602085706, -0.081623956880000403, 176.15408567057878, {kSub});
    for (int step = 0; step < 5; ++step) {
      zoomIn();
    }
    checkState(state, kSub, 0.0046277212760469644, 0.0070537468208247689, 537.57960714898309, {});
    const auto zoomOut = [&] { state = zoomAt(world.source, state.cwd, state.camera, view, 1 / 1.6, 300, 300); };
    for (int step = 0; step < 3; ++step) {
      zoomOut();
    }
    checkState(state, kSub, 0.0046277212760469644, 0.0070537468208247689, 131.24502127660719, {});
    zoomOut();
    checkState(state, kDocs, 0.033580869949217197, 0.051185224976892853, 635.04757008558954, {kDocs});
    for (int step = 0; step < 4; ++step) {
      zoomOut();
    }
    checkState(state, kHome, -0.39470953081649701, 0.085023131834048193, 198.21188805460949, {kHome});
    zoomOut();
    zoomOut();
    checkState(state, kTop, -0.56046603207051116, 0.10769320315971236, 171.6, {kTop});
    zoomOut();
    checkState(state, kTop, -0.56046603207051116, 0.10769320315971236, 171.6, {});
  }

  // On a tall narrow pane a small pan or zoom out keeps the folder; the larger side would have surfaced it.
  void testTallNarrowPaneKeepsTheFolder() {
    struct Case {
      ViewSize view;
      double rest;
      double panned;
    };
    for (const auto& [view, rest, panned] :
         {Case{{340, 600}, 156, 0.19230769230769232}, Case{{600, 340}, 156, 0.19230769230769232},
          Case{{260, 700}, 116, 0.25862068965517238}}) {
      World world;
      const double w = view.width / 2;
      const double h = view.height / 2;
      auto state = zoomAt(world.source, kDocs, restCamera(view), view, 0.9, w, h);
      checkState(state, kDocs, 0, 0, rest * 0.9, {});
      state = zoomAt(world.source, state.cwd, state.camera, view, 1 / 0.9, w, h);
      auto camera = state.camera;
      camera.cx += 30 / camera.scale;
      state = afterMove(world.source, kDocs, camera, view, false, 0, 0);
      checkState(state, kDocs, panned, 0, rest, {});
      state = zoomAt(world.source, state.cwd, state.camera, view, 0.5, w, h);
      checkState(state, kDocs, panned, 0, rest / 2, {});
    }
  }

  void testClampCamera() {
    World world;
    const ViewSize view{600, 600};
    checkCamera(
        clampCamera(world.source, kTop, {.cx = 0, .cy = 0, .scale = 50}, view, 10, 20), 0.14172494172494174,
        0.28344988344988348, 171.6
    );
    checkCamera(clampCamera(world.source, kTop, {.cx = 3, .cy = 0, .scale = 286}, view, 0, 0), 1.1, 0, 286);
    checkCamera(
        clampCamera(world.source, kDocs, {.cx = 5, .cy = 1, .scale = 286}, view, 0, 0), 2.7565493126947516,
        0.39438046633342194, 286
    );
    const auto sub = placed(world, kDocs, kSub);
    const auto deep = clampCamera(world.source, kDocs, {.cx = sub->x, .cy = sub->y, .scale = 200000}, view, 15, -25);
    checkCamera(deep, 0.032458382308590515, 0.051148656209703838, 25011.177813333008);
    const auto levels = probe(world.source, kDocs, deep);
    TEST_CHECK(levels.size() == 2 && levels[1].node == kFirstS);
    TEST_NEAR(levels[1].r, 1020.0, 1e-9);
    TEST_CHECK(clampCamera(world.source, kDocs, {.cx = kNan, .cy = 0, .scale = 286}, view, 0, 0) == restCamera(view));
    TEST_CHECK(clampCamera(world.source, kDocs, {.cx = 0, .cy = 0, .scale = -1}, view, 0, 0) == restCamera(view));
    // A zoom limit with an offset that is not finite keeps the view center fixed.
    checkCamera(clampCamera(world.source, kTop, {.cx = 0, .cy = 0, .scale = 50}, view, kNan, 0), 0, 0, 171.6);
    checkCamera(clampCamera(world.source, kTop, {.cx = 0, .cy = 0, .scale = 50}, view, 0, -kInfinity), 0, 0, 171.6);
    const auto steady = zoomAt(world.source, kDocs, restCamera(view), view, 1.25, kNan, 300);
    checkState(steady, kDocs, 0, 0, 357.5, {});
  }

  void testSettleCases() {
    World world;
    const ViewSize view{600, 600};
    // Zooming in with the view center outside the current circle surfaces, then enters the sibling.
    checkState(
        zoomAt(world.source, kDocs, {.cx = 0, .cy = 0, .scale = 200}, view, 4, 590, 300), kMusic, -0.89595304404480514,
        0, 672.77316628218841, {kHome, kMusic}
    );
    // Panning outside without zooming in keeps the folder.
    checkState(
        afterMove(world.source, kDocs, {.cx = 1.05, .cy = 0, .scale = 300}, view, false, 0, 0), kDocs, 1.05, 0, 300, {}
    );
    // One large zoom dives through several levels in one settle.
    const auto home = placed(world, kTop, kHome);
    checkState(
        zoomAt(world.source, kTop, {.cx = home->x - 0.05, .cy = home->y, .scale = 286}, view, 12, 300, 300), kDocs,
        0.65863818310426803, -0.12273114759148016, 941.15965522327622, {kHome, kDocs}
    );
    // Degenerate input leaves the state alone or clamps it.
    const Camera broken{.cx = kNan, .cy = 0, .scale = 286};
    const auto settled = settle(world.source, kDocs, broken, view, true);
    TEST_CHECK(settled.cwd == kDocs && settled.steps.empty() && std::isnan(settled.camera.cx));
    TEST_CHECK(afterMove(world.source, kDocs, broken, view, true, 0, 0).camera == restCamera(view));
    const auto unknown = settle(world.source, 9999, restCamera(view), view, true);
    TEST_CHECK(unknown.cwd == 9999 && unknown.steps.empty());
    const auto still = zoomAt(world.source, kDocs, restCamera(view), view, kNan, 300, 300);
    TEST_CHECK(still.cwd == kDocs && still.camera == restCamera(view));
  }

  // At most 16 promotions per settle, even when every step qualifies.
  void testSettleIsBounded() {
    std::vector<std::string> names;
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "n0", .folder = true, .bytes = 1}};
    names.reserve(40);
    for (int i = 1; i < 40; ++i) {
      names.push_back(std::format("n{}", i));
    }
    for (int i = 1; i < 40; ++i) {
      rows.push_back({.parent = i - 1, .name = names[static_cast<std::size_t>(i - 1)], .folder = true, .bytes = 1});
    }
    const auto chainTree = workbenchTree(rows, 0);
    LayoutSettings settings;
    PackingCache packings;
    const AtlasSource source{.tree = chainTree, .settings = settings, .packings = packings};
    // Each folder holds one child at (0, 0.06) with radius 0.9, so y = 0.6 maps onto itself at every level.
    const auto state = settle(source, 0, {.cx = 0, .cy = 0.6, .scale = 1e9}, {600, 600}, true);
    TEST_CHECK(state.steps.size() == 16);
    TEST_CHECK(state.cwd == 16);
  }

  void testCursorReveal() {
    World world;
    const ViewSize view{600, 600};
    const Camera rest = restCamera(view);
    const Camera right{.cx = 0.5, .cy = 0, .scale = 900};
    const Camera small{.cx = 0, .cy = 0, .scale = 40};
    TEST_CHECK(!cursorNeedsReveal(world.source, kDocs, kFirstFile, rest, view));
    TEST_CHECK(!cursorNeedsReveal(world.source, kDocs, kFirstFile, right, view));
    TEST_CHECK(cursorNeedsReveal(world.source, kDocs, kFirstFile, small, view));
    TEST_CHECK(!cursorNeedsReveal(world.source, kDocs, kFirstFile + 58, rest, view));
    TEST_CHECK(cursorNeedsReveal(world.source, kDocs, kFirstFile + 58, right, view));
    TEST_CHECK(cursorNeedsReveal(world.source, kDocs, kSub, right, view));
    TEST_CHECK(!cursorNeedsReveal(world.source, kDocs, kFirstFile + 65, small, view));
    TEST_CHECK(!cursorNeedsReveal(world.source, kDocs, kNoNode, small, view));
    TEST_CHECK(!cursorNeedsReveal(world.source, 9999, kFirstFile, small, view));
    // The bubble's center 25 px from the left edge is inside the 30 px margin; at 35 px it is not.
    TEST_CHECK(
        cursorNeedsReveal(world.source, kDocs, kFirstFile, {.cx = 1.2323209612550527, .cy = 0, .scale = 286}, view)
    );
    TEST_CHECK(
        !cursorNeedsReveal(world.source, kDocs, kFirstFile, {.cx = 1.1973559262900177, .cy = 0, .scale = 286}, view)
    );
  }

} // namespace

int main() {
  testProbe();
  testDiveAndSurface();
  testTallNarrowPaneKeepsTheFolder();
  testClampCamera();
  testSettleCases();
  testSettleIsBounded();
  testCursorReveal();
  std::println("workbench atlas navigation checks passed");
  return 0;
}
