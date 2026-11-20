#include "atlas_view_rig.h"
#include "instrument_rig.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <chrono>
#include <linux/input-event-codes.h>

// A click on a folder's circle enters it: once the page gives the Atlas the entered folder, the map flies to rest
// there, even when the page publishes that folder again during the flight.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;
  using namespace luciole::test::atlas_view;
  using luciole::test::workbenchTree;

  constexpr double kMiB = 1048576;
  // Ids: 0 top, 1 work, 2 projects, 3 archive, 4 media, 5 disk.img, 6 alpha, 7 beta, 8 gamma (in projects),
  // 9 video, 10 photos (in media).
  constexpr NodeId kWork = 1;
  constexpr NodeId kProjects = 2;
  constexpr NodeId kAlpha = 6;

  Tree workTree(NodeId current) {
    auto tree = workbenchTree(
        {
            {.parent = -1, .name = "top", .folder = true, .bytes = 300 * kMiB, .items = 1},
            {.parent = 0, .name = "work", .folder = true, .bytes = 205 * kMiB, .items = 4, .files = 1},
            {.parent = 1, .name = "projects", .folder = true, .bytes = 85 * kMiB, .items = 3},
            {.parent = 1, .name = "archive", .folder = true, .bytes = 70 * kMiB, .items = 1, .files = 1},
            {.parent = 1, .name = "media", .folder = true, .bytes = 46 * kMiB, .items = 2},
            {.parent = 1, .name = "disk.img", .bytes = 3 * kMiB},
            {.parent = 2, .name = "alpha", .folder = true, .bytes = 76 * kMiB},
            {.parent = 2, .name = "beta", .folder = true, .bytes = 9 * kMiB},
            {.parent = 2, .name = "gamma", .folder = true, .bytes = 0.3 * kMiB},
            {.parent = 4, .name = "video", .folder = true, .bytes = 40 * kMiB},
            {.parent = 4, .name = "photos", .folder = true, .bytes = 6 * kMiB},
        },
        6000
    );
    tree.root = current;
    tree.ancestors = parentChain(tree, current);
    return tree;
  }

  // Lets worker results reach the main context, without a frame, until the view asks for one: a layout landed.
  void landed(Rig& rig, std::chrono::milliseconds limit = kWaitLimit) {
    const auto asked = rig.frameRequests;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (rig.frameRequests == asked) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      pump(1);
    }
    rig.frame(rig.view->inputs());
  }

  void clickingACircleEntersAndFlies() {
    const auto from = inputsFor(workTree(kWork), 1);
    Rig rig(WorkbenchInstrument::Atlas, 340, 470);
    rig.show(from);
    settle(rig, from);
    const auto& view = rig.view->shapeView();
    const auto at = centerOf(view, kProjects);
    rig.click(at, BTN_LEFT);
    TEST_CHECK(rig.requests.size() == 1);
    TEST_CHECK(rig.requests[0].kind == PartRequestKind::Go && rig.requests[0].node == kProjects);
    const auto entered = inputsFor(workTree(kProjects), 2);
    rig.view->setCursor(kAlpha);
    rig.frame(entered);
    // The entered folder's layout lands and its flight starts, with no frame to move it yet.
    landed(rig);
    const float start = radiusOf(view, kProjects);
    // The page publishes the same folder again before the flight moved, as a finished measure or a recorded visit
    // does; its layout lands too before the next frame.
    const auto again = inputsFor(workTree(kProjects), 3);
    rig.frame(again);
    landed(rig);
    TEST_CHECK(rig.view->shownSnapshot().tree == again.snapshot.tree);
    settle(rig, again);
    Rig fresh(WorkbenchInstrument::Atlas, 340, 470);
    fresh.show(entered);
    settle(fresh, entered);
    const auto& rest = fresh.view->shapeView();
    TEST_CHECK(start < radiusOf(rest, kProjects) * 0.9F); // the map had a way to fly
    TEST_NEAR(radiusOf(view, kProjects), radiusOf(rest, kProjects), 0.01F);
    TEST_NEAR(centerOf(view, kProjects).x, centerOf(rest, kProjects).x, 0.01F);
    TEST_NEAR(centerOf(view, kProjects).y, centerOf(rest, kProjects).y, 0.01F);
  }
} // namespace

int main() {
  clickingACircleEntersAndFlies();
  return 0;
}
