#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/atlas_part.h"
#include "ui/workbench/scene_part.h"
#include "workbench/circle_pack.h"
#include "workbench/tree.h"
#include "workbench_fixture.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <glib.h>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  namespace fixture = luciole::test;

  constexpr double kWidth = 420;
  constexpr double kHeight = 380;

  PartFrame frameFor(std::shared_ptr<const Tree> tree, std::uint64_t serial) {
    return {
        .tree = std::move(tree),
        .serial = serial,
        .settings = {},
        .palette = {},
        .width = kWidth,
        .height = kHeight,
        .metrics = {},
    };
  }

  // Lets worker results reach the main context until `done`, at most `turns` times about a millisecond apart.
  template <typename Done> void pumpUntil(Done done, int turns = 4000) {
    for (int turn = 0; turn < turns && !done(); ++turn) {
      while (g_main_context_iteration(nullptr, FALSE) != 0) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(done());
  }

  // Processor time of the calling thread.
  double threadMs() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
  }

  // r holds folder a, a holds folder b, b holds f.txt: each folder fills its parent.
  std::shared_ptr<const Tree> chainTree() {
    return std::make_shared<const Tree>(fixture::workbenchTree(
        {
            {.parent = -1, .name = "r", .folder = true, .bytes = 1000, .items = 1, .files = 1},
            {.parent = 0, .name = "a", .folder = true, .bytes = 1000, .items = 1, .files = 1},
            {.parent = 1, .name = "b", .folder = true, .bytes = 1000, .items = 1, .files = 1},
            {.parent = 2, .name = "f.txt", .bytes = 1000},
        },
        6000
    ));
  }

  // Folder r with `folders` folders d0, d1, ... of `files` files each, entered at `root`.
  Tree wideTree(std::size_t folders, std::size_t files, NodeId root) {
    Tree tree;
    tree.now = 6000;
    tree.nodes.push_back({.name = "r", .folder = true});
    tree.nodes[0].childCount = static_cast<std::uint32_t>(folders);
    for (std::size_t i = 0; i < folders; ++i) {
      tree.children.push_back(static_cast<NodeId>(1 + i));
      tree.nodes.push_back({
          .name = "d" + std::to_string(i),
          .parent = 0,
          .bytes = 1000.0 + static_cast<double>(i % 97) * 31,
          .files = static_cast<double>(files),
          .items = static_cast<double>(files),
          .folder = true,
      });
    }
    for (std::size_t i = 0; i < folders; ++i) {
      tree.nodes[1 + i].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[1 + i].childCount = static_cast<std::uint32_t>(files);
      for (std::size_t j = 0; j < files; ++j) {
        tree.children.push_back(static_cast<NodeId>(tree.nodes.size()));
        tree.nodes.push_back({
            .name = "f" + std::to_string(j) + ".txt",
            .parent = static_cast<NodeId>(1 + i),
            .bytes = 10.0 + static_cast<double>(j % 13),
        });
      }
    }
    tree.nodes[0].items = static_cast<double>(folders);
    tree.root = root;
    tree.ancestors = parentChain(tree, root);
    return tree;
  }

  // A layout's reply that reaches the main context after its part is gone runs nothing of the part.
  void aDestroyedPartNeverReports() {
    int reports = 0;
    auto part = makeAtlasPart([&reports] { ++reports; });
    TEST_CHECK(!part->layout(frameFor(chainTree(), 1)));
    // The worker takes the job well within this; its reply then waits for the main context.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    part.reset();
    for (int turn = 0; turn < 200; ++turn) {
      g_main_context_iteration(nullptr, FALSE);
    }
    TEST_CHECK(reports == 0);
    // The same wait delivers the reply of a part that stays.
    auto kept = makeAtlasPart([&reports] { ++reports; });
    TEST_CHECK(!kept->layout(frameFor(chainTree(), 1)));
    pumpUntil([&reports] { return reports == 1; });
  }

  // Until the layout of a new snapshot returns, the part shows, and speaks in, the one before.
  void theShownSnapshotHoldsUntilItsLayoutReturns() {
    int reports = 0;
    auto part = makeAtlasPart([&reports] { ++reports; });
    TEST_CHECK(part->shownSnapshot().tree == nullptr);
    const auto first = chainTree();
    const auto second = chainTree();
    TEST_CHECK(!part->layout(frameFor(first, 1)));
    pumpUntil([&reports] { return reports == 1; });
    TEST_CHECK(part->shownSnapshot().tree == first && part->shownSnapshot().serial == 1);
    TEST_CHECK(!part->layout(frameFor(second, 2)));
    TEST_CHECK(part->shownSnapshot().tree == first && part->shownSnapshot().serial == 1);
    TEST_CHECK(!part->shapes({}).shapes.empty());
    pumpUntil([&reports] { return reports == 2; });
    TEST_CHECK(part->shownSnapshot().tree == second && part->shownSnapshot().serial == 2);
  }

  // Frames 16 ms apart until the part stops moving, at most 500; the requests they made, in order.
  std::vector<PartRequest> runFrames(ScenePart& part) {
    std::vector<PartRequest> requests;
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    for (int frame = 0; frame < 500 && part.moving(); ++frame) {
      now += std::chrono::milliseconds(16);
      const auto reaction = part.advance(now);
      TEST_CHECK(reaction.handled && !reaction.relayout);
      requests.insert(requests.end(), reaction.requests.begin(), reaction.requests.end());
    }
    TEST_CHECK(!part.moving());
    return requests;
  }

  // A turn's zoom lands over the next frames, which ask for each folder a dive or a surfacing passes, in order.
  void aWheelTurnGoesToEachFolderOnTheWay() {
    int reports = 0;
    auto part = makeAtlasPart([&reports] { ++reports; });
    TEST_CHECK(!part->layout(frameFor(chainTree(), 1)));
    pumpUntil([&reports] { return reports == 1; });
    TEST_CHECK(!part->moving());
    const ShapeWheel in{.vertical = true, .delta = -10, .x = kWidth / 2, .y = kHeight / 2};
    const auto turn = part->wheel(in);
    TEST_CHECK(turn.handled && !turn.relayout && turn.requests.empty());
    TEST_CHECK(part->moving());
    const auto dive = runFrames(*part);
    TEST_CHECK(dive.size() == 2);
    TEST_CHECK(dive.size() == 2 && dive[0].node == 1 && dive[1].node == 2);
    std::vector<PartRequest> surfaced;
    const ShapeWheel out{.vertical = true, .delta = 400, .x = kWidth / 2, .y = kHeight / 2};
    for (int turn = 0; turn < 20 && (surfaced.empty() || surfaced.back().node != 0); ++turn) {
      TEST_CHECK(part->wheel(out).handled);
      surfaced.append_range(runFrames(*part));
    }
    TEST_CHECK(surfaced.size() == 2);
    TEST_CHECK(surfaced.size() == 2 && surfaced[0].node == 1 && surfaced[1].node == 0);
    for (const auto& request : surfaced) {
      TEST_CHECK(request.kind == PartRequestKind::Go);
    }
  }

  // Taking up a snapshot in another folder, and the first wheel turn after it with the frame that applies it, cost the
  // main context far less than packing the large folder they need, which the layout job does.
  void theMainContextPacksNoLargeFolder() {
    const auto top = std::make_shared<const Tree>(wideTree(1500, 12, 0));
    const auto entered = std::make_shared<const Tree>(wideTree(1500, 12, 1));
    PackingCache cold;
    const double packStart = threadMs();
    TEST_CHECK(!cold.packing(*entered, 0, LayoutSettings{}, {})->items.empty());
    const double pack = threadMs() - packStart;
    int reports = 0;
    auto part = makeAtlasPart([&reports] { ++reports; });
    TEST_CHECK(!part->layout(frameFor(top, 1)));
    pumpUntil([&reports] { return reports == 1; });
    const double adoptStart = threadMs();
    TEST_CHECK(!part->layout(frameFor(entered, 2)));
    const double adopt = threadMs() - adoptStart;
    TEST_CHECK(adopt < pack / 4);
    pumpUntil([&reports] { return reports == 2; });
    TEST_CHECK(part->shownSnapshot().tree == entered);
    const double wheelStart = threadMs();
    TEST_CHECK(part->wheel({.vertical = true, .delta = -10, .x = kWidth / 2, .y = kHeight / 2}).handled);
    const auto frame = part->advance(std::chrono::steady_clock::now());
    const double wheel = threadMs() - wheelStart;
    TEST_CHECK(frame.handled);
    TEST_CHECK(wheel < pack / 4);
  }
} // namespace

int main() {
  aDestroyedPartNeverReports();
  theShownSnapshotHoldsUntilItsLayoutReturns();
  aWheelTurnGoesToEachFolderOnTheWay();
  theMainContextPacksNoLargeFolder();
  return 0;
}
