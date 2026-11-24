#include "cpu_budget.h"
#include "instrument_rig.h"
#include "machine_slowdown.h"
#include "metrics_renderer.h"
#include "test_check.h"
#include "ui/workbench/instrument_view.h"
#include "ui/workbench/nest_part.h"
#include "ui/workbench/scene_part.h"
#include "workbench/color.h"
#include "workbench/scene_shapes.h"
#include "workbench/tree.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <glib.h>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// What the Nest costs the main context on a folder of 10,000 entries, in the processor time of the thread that runs the
// main context (never wall-clock time, never the time of other threads or processes): taking up a snapshot (starting
// its layout, taking up the scene and then its texture, and making the shapes of each) and a hover change (making the
// shapes again). Layout, texture and peek run on workers, whose time does not count; the main context's processor time
// does. The part's own work is held to the budget; the whole instrument view, whose shape view, labels and snapshot
// checks are shared with the other parts, is reported beside it.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using namespace luciole::test::instrument;

  constexpr std::size_t kEntries = 10000;
  constexpr double kBudgetMs = 2.0;
  constexpr int kQuietTurns = 40; // turns without a reply before the view counts as settled
  constexpr std::size_t kHovers = 24;

  double threadMs() { return luciole::test::referenceThreadMs(); }

  // The thread time since `start`, after spending (stretch - 1) times as much again on the thread: a deliberate
  // slowdown of the measured code, used only to show that the bounds notice one.
  double stretched(double start, double stretch) {
    const double target = (threadMs() - start) * stretch;
    while (threadMs() - start < target) {
    }
    return threadMs() - start;
  }

  // top > mid > here, each level with siblings; here holds kEntries entries, every `folderEvery`-th one (none for 0) a
  // folder of `files` files.
  Tree tenThousand(std::size_t folderEvery, std::size_t files) {
    Tree tree;
    tree.now = 1e9;
    std::vector<std::vector<NodeId>> kids;
    const auto add = [&](std::string name, NodeId parent, bool folder, double bytes) {
      workbench::Node node;
      node.name = std::move(name);
      node.parent = parent;
      node.folder = folder;
      node.bytes = bytes;
      node.modified = 1e9 - 3600;
      node.category = static_cast<std::uint8_t>(fileCategory(node.name, folder));
      tree.nodes.push_back(std::move(node));
      kids.emplace_back();
      const auto id = static_cast<NodeId>(tree.nodes.size() - 1);
      if (parent != kNoNode) {
        kids[parent].push_back(id);
      }
      return id;
    };
    const auto top = add("top", kNoNode, true, 1e12);
    const auto mid = add("mid", top, true, 1e11);
    for (std::size_t i = 0; i < 40; ++i) {
      add("s" + std::to_string(i), top, i % 3 == 0, 1e8 + static_cast<double>(i));
    }
    const auto here = add("here", mid, true, 1e10);
    for (std::size_t i = 0; i < 60; ++i) {
      add("m" + std::to_string(i), mid, i % 4 == 0, 1e7 + static_cast<double>(i));
    }
    for (std::size_t i = 0; i < kEntries; ++i) {
      const bool folder = folderEvery > 0 && i % folderEvery == 0;
      const auto id =
          add((folder ? "d" : "f") + std::to_string(i) + (folder ? "" : ".txt"), here, folder,
              1000.0 + static_cast<double>((i * 7919) % 100000));
      for (std::size_t k = 0; folder && k < files; ++k) {
        add("k" + std::to_string(k) + ".png", id, false, 10.0 + static_cast<double>((k * 31) % 997));
      }
    }
    for (std::size_t id = 0; id < tree.nodes.size(); ++id) {
      tree.nodes[id].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[id].childCount = static_cast<std::uint32_t>(kids[id].size());
      tree.nodes[id].items = static_cast<double>(kids[id].size());
      tree.children.insert(tree.children.end(), kids[id].begin(), kids[id].end());
    }
    tree.root = here;
    tree.ancestors = parentChain(tree, here);
    return tree;
  }

  // Main-context time from taking up `inputs` until the view is settled: the frame that starts the layout, and each
  // turn of the main loop that delivers a worker's reply, asks for a frame or follows a tween, with the frame after
  // it. Other turns cost nothing in the application, which sleeps in poll, so they do not count. Tweens run to their
  // end between turns; their frames are not part of taking up a snapshot.
  double takeUp(Rig& rig, const PartInputs& inputs) {
    double spent = 0;
    double start = threadMs();
    rig.frame(inputs);
    spent += threadMs() - start;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int quiet = 0; quiet < kQuietTurns || rig.view->shownSnapshot().serial != inputs.snapshot.serial;) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      const bool tweened = rig.animations.hasActive();
      rig.finishTransition();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      const auto asked = rig.frameRequests;
      start = threadMs();
      bool delivered = false;
      while (g_main_context_iteration(nullptr, FALSE) != 0) {
        delivered = true;
      }
      rig.frame(inputs);
      const double turn = threadMs() - start;
      const bool counted = delivered || tweened || rig.frameRequests != asked;
      spent += counted ? turn : 0;
      quiet = counted ? 0 : quiet + 1;
    }
    return spent;
  }

  struct Cost {
    double first = 0;  // the first snapshot
    double second = 0; // the next one
    double hoverMean = 0;
    double hoverMost = 0;
    std::size_t shapes = 0;
    double slowdown = 1; // the machine slowdown the costs were divided by, at most over the repeats
  };

  // The part alone, as its owner drives it: one layout per snapshot, and its shapes made again after each report.
  struct PartRun {
    std::unique_ptr<ScenePart> part = makeNestPart();
    int reports = 0;
    std::size_t shapes = 0;
    double stretch = 1;

    PartRun() {
      part->connect([this] { ++reports; }, [this](float) { ++reports; });
    }

    PartFrame frameFor(const PartInputs& inputs, float width, float height) const {
      PartFrame frame{
          .tree = inputs.snapshot.tree,
          .serial = inputs.snapshot.serial,
          .settings = {},
          .palette = {},
          .width = width,
          .height = height,
          .metrics = {},
      };
      ::test::MetricsRenderer renderer;
      frame.metrics.measure(renderer);
      return frame;
    }

    // Main-context time of the layout call, of each turn that delivers a reply, and of the shapes each report asks
    // for, until nothing arrives for a while.
    double takeUp(const PartInputs& inputs, float width, float height) {
      const auto frame = frameFor(inputs, width, height);
      double start = threadMs();
      part->layout(frame);
      double spent = stretched(start, stretch);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      for (int quiet = 0; quiet < kQuietTurns || part->shownSnapshot().serial != inputs.snapshot.serial;) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int before = reports;
        start = threadMs();
        bool delivered = false;
        while (g_main_context_iteration(nullptr, FALSE) != 0) {
          delivered = true;
        }
        if (reports != before) {
          shapes = part->shapes({}).shapes.size();
        }
        const double turn = stretched(start, stretch);
        spent += delivered ? turn : 0;
        quiet = delivered ? 0 : quiet + 1;
      }
      return spent;
    }
  };

  Cost measurePart(const Tree& tree, float width, float height, double stretch = 1) {
    const auto first = inputsFor(tree, 1);
    const auto second = inputsFor(tree, 2);
    PartRun run;
    run.stretch = stretch;
    Cost cost;
    cost.first = run.takeUp(first, width, height);
    cost.second = run.takeUp(second, width, height);
    cost.shapes = run.shapes;
    const auto scene = run.part->shapes({});
    std::vector<ShapeId> tiles;
    for (const auto& shape : scene.shapes) {
      if (shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece && shape.kind == ShapeKind::RoundedRect) {
        tiles.push_back(shape.id);
      }
    }
    TEST_CHECK(tiles.size() >= 2);
    double total = 0;
    for (std::size_t i = 1; i <= kHovers; ++i) {
      const auto id = tiles[i % tiles.size()];
      const double start = threadMs();
      run.part->hover(id);
      const auto shapes = run.part->shapes({.hovered = id, .linked = kNoNode, .cursor = kNoNode});
      const double spent = stretched(start, stretch);
      TEST_CHECK(!shapes.shapes.empty());
      total += spent;
      cost.hoverMost = std::max(cost.hoverMost, spent);
    }
    run.part->hover(std::nullopt);
    cost.hoverMean = total / static_cast<double>(kHovers);
    return cost;
  }

  // The snapshots outlive the view, so freeing a replaced one, which their producer does, is not counted here.
  Cost measureView(const Tree& tree, float width, float height) {
    const auto first = inputsFor(tree, 1);
    const auto inputs = inputsFor(tree, 2);
    Rig rig(WorkbenchInstrument::Nest, width, height);
    Cost cost;
    cost.first = takeUp(rig, first);
    cost.second = takeUp(rig, inputs);
    rig.frame(inputs);
    cost.shapes = rig.view->shapeView().shapes().size();
    std::vector<ShapePoint> tiles;
    for (const auto& shape : rig.view->shapeView().shapes()) {
      if (shape.hittable && sceneRole(shape.id.role) == SceneRole::Piece && shape.kind == ShapeKind::RoundedRect) {
        tiles.push_back(inside(shape));
      }
    }
    TEST_CHECK(tiles.size() >= 2);
    rig.dispatcher.pointerEnter(tiles[0].x, tiles[0].y, 1);
    rig.frame(inputs);
    double total = 0;
    for (std::size_t i = 1; i <= kHovers; ++i) {
      const auto at = tiles[i % tiles.size()];
      const double start = threadMs();
      rig.dispatcher.pointerMotion(at.x, at.y, 1);
      rig.frame(inputs);
      const double spent = threadMs() - start;
      total += spent;
      cost.hoverMost = std::max(cost.hoverMost, spent);
    }
    cost.hoverMean = total / static_cast<double>(kHovers);
    return cost;
  }

  double worstOf(const Cost& cost) { return std::max({cost.first, cost.second, cost.hoverMost}); }

  Cost scaled(Cost cost, double slowdown) {
    cost.first /= slowdown;
    cost.second /= slowdown;
    cost.hoverMean /= slowdown;
    cost.hoverMost /= slowdown;
    cost.slowdown = slowdown;
    return cost;
  }

  // A cost that every other cost is under, to start `keepLeast` from.
  Cost leastPossible() {
    constexpr double kMost = 1e9;
    return {.first = kMost, .second = kMost, .hoverMean = kMost, .hoverMost = kMost, .shapes = 0, .slowdown = 1};
  }

  // `best` takes the least of each cost of `cost`: another process sharing the core only ever adds time.
  void keepLeast(Cost& best, const Cost& cost) {
    best.first = std::min(best.first, cost.first);
    best.second = std::min(best.second, cost.second);
    best.hoverMean = std::min(best.hoverMean, cost.hoverMean);
    best.hoverMost = std::min(best.hoverMost, cost.hoverMost);
    best.slowdown = std::max(best.slowdown, cost.slowdown);
  }

  // One run, divided by the slowdown measured just before it.
  template <typename Measure> Cost measuredOnce(Measure measure) {
    const auto slowdown = luciole::test::machineSlowdown();
    return scaled(measure(), slowdown);
  }

  // The least of each cost over kRepeats runs.
  template <typename Measure> Cost least(Measure measure) {
    constexpr int kRepeats = 5;
    Cost best = measuredOnce(measure);
    for (int i = 1; i < kRepeats; ++i) {
      keepLeast(best, measuredOnce(measure));
    }
    return best;
  }

  void report(const char* what, const char* name, std::array<float, 2> size, const Cost& cost) {
    std::println(
        "  {} {} at {}x{}: snapshot {:.2f} ms, next snapshot {:.2f} ms, hover {:.2f} ms (at most {:.2f}), {} shapes, "
        "machine slowdown {:.2f}",
        what, name, size[0], size[1], cost.first, cost.second, cost.hoverMean, cost.hoverMost, cost.shapes,
        cost.slowdown
    );
  }

  void eachSizeStaysWithinTheBudget() {
    struct Case {
      const char* name;
      std::size_t folderEvery;
      std::size_t files;
    };
    constexpr std::array<Case, 3> kCases{{
        {.name = "files only", .folderEvery = 0, .files = 0},
        {.name = "a folder in ten", .folderEvery = 10, .files = 30},
        {.name = "folders only", .folderEvery = 1, .files = 20},
    }};
    constexpr std::array<std::array<float, 2>, 3> kSizes{{{340, 534}, {1400, 900}, {2560, 1400}}};
    double worst = 0;
    for (const auto& each : kCases) {
      const auto tree = tenThousand(each.folderEvery, each.files);
      for (const auto& size : kSizes) {
        const auto part = least([&] { return measurePart(tree, size[0], size[1]); });
        report("part", each.name, size, part);
        worst = std::max(worst, worstOf(part));
        const auto view = least([&] { return measureView(tree, size[0], size[1]); });
        report("view", each.name, size, view);
        // Protects against a new snapshot costing the view more than the first did, as when it compares more than the
        // ids it draws.
        TEST_CHECK(view.second < 1.5 * view.first + 0.5);
      }
    }
    // Protects the main context against a frame spent on one snapshot or hover change: the dearest of them, in
    // thread processor time, divided by the machine slowdown, stays under the budget.
    std::println("  dearest part cost {:.2f} ms against the {:.2f} ms budget", worst, kBudgetMs);
    TEST_CHECK(TEST_WITHIN_BUDGET(worst, kBudgetMs));
  }

  // The bound is on the part's own processor time: a part made kStretch times slower goes over it, and the division by
  // the machine slowdown does not hide that. The stretch is built in, so the slowed part's cost is kStretch times the
  // cost the same run measured before stretching; what the check can get wrong is only that cost against the plain
  // part's, two runs a moment apart. The ratio compares processor times as measured, not divided by a slowdown that
  // is estimated apart from the part's own work and adds its own error to each side. Runs of the plain and the slowed
  // part alternate, so load that comes and goes reaches both alike, and each side keeps its least over kRounds
  // rounds. The ratio must reach kLeastRatio of the stretch: it measured 17.7 to 20.5 of 20 over 15 runs, alone and on
  // a machine kept busy by four other tests, so the limit of 15 leaves room beyond that, while a part slowed only half
  // as much, the case this exists to catch, measures about 10 and fails.
  void aSlowerPartGoesOverTheBudget() {
    constexpr double kStretch = 20;
    constexpr double kLeastRatio = 0.75;
    constexpr int kRounds = 8;
    const auto tree = tenThousand(0, 0);
    Cost rawBase = leastPossible();
    Cost rawSlow = leastPossible();
    Cost base = leastPossible();
    Cost slow = leastPossible();
    for (int round = 0; round < kRounds; ++round) {
      const auto plain = measurePart(tree, 1400, 900);
      const auto slowed = measurePart(tree, 1400, 900, kStretch);
      const auto slowdown = luciole::test::machineSlowdown();
      keepLeast(rawBase, plain);
      keepLeast(rawSlow, slowed);
      keepLeast(base, scaled(plain, slowdown));
      keepLeast(slow, scaled(slowed, slowdown));
    }
    const double ratio = worstOf(rawSlow) / worstOf(rawBase);
    std::println(
        "  part files only at 1400x900: {:.2f} ms, made {}x slower {:.2f} ms (as measured {:.2f} and {:.2f} ms, ratio "
        "{:.2f})",
        worstOf(base), kStretch, worstOf(slow), worstOf(rawBase), worstOf(rawSlow), ratio
    );
    TEST_CHECK(ratio >= kLeastRatio * kStretch);
    TEST_CHECK(worstOf(slow) >= kBudgetMs);
  }
} // namespace

int main() {
  aSlowerPartGoesOverTheBudget();
  eachSizeStaysWithinTheBudget();
  std::println("nest part cost checks passed");
  return 0;
}
