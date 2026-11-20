#include "test_check.h"
#include "workbench/ring_geometry.h"
#include "workbench/ring_leaders.h"
#include "workbench_ring_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench::ring;
  using namespace luciole::test::ring;

  void testLaneLeadersTurnTheShortWayFromAnyAngle() {
    const auto dial = fullDial(1400, 737).value();
    const std::vector<LeaderItem> items{
        {.degrees = 765, .y = 300, .side = Side::Right}, {.degrees = -300, .y = 330, .side = Side::Right}
    };
    const auto paths = routeLanes(dial, items);
    TEST_CHECK(paths.size() == 2);
    TEST_CHECK(paths[0].size() == 20 && paths[1].size() == 13);
    TEST_NEAR(paths[0][1].x, 916.5514517383803, 1e-6);
    TEST_NEAR(paths[0][1].y, 192.44854826161992, 1e-6);
    TEST_NEAR(paths[0][18].x, 986.1958464059182, 1e-6);
    TEST_NEAR(paths[0][18].y, 300.0, 1e-6);
    TEST_NEAR(paths[1][1].x, 963.2717227504693, 1e-6);
    TEST_NEAR(paths[1][11].x, 993.5557868617138, 1e-6);
    TEST_NEAR(paths[1][11].y, 330.0, 1e-6);
    const auto broken = routeLanes(dial, std::vector{LeaderItem{.degrees = kNan, .y = 300, .side = Side::Left}});
    TEST_CHECK(broken.size() == 1 && broken[0].size() == 4);
    TEST_CHECK((broken[0].back() == Point{370, 300}));
  }

  struct Scenario {
    Dial dial;
    std::vector<LeaderItem> right;
    std::vector<LeaderItem> left;
    LeaderRoutes routes;
    std::vector<Polyline> lanes;
  };

  // Both columns built the way the scene builds them.
  Scenario scenario(double width, double height, const std::vector<double>& weights) {
    Scenario result{.dial = fullDial(width, height).value(), .right = {}, .left = {}, .routes = {}, .lanes = {}};
    const auto fit = fitCount(weights, result.dial.rowsFit, 1.5);
    const auto middles = mids(fit.angles);
    for (std::size_t i = 0; i < middles.size(); ++i) {
      const bool right = i < fit.split;
      (right ? result.right : result.left)
          .push_back({.degrees = middles[i], .y = 0, .side = right ? Side::Right : Side::Left});
    }
    std::ranges::reverse(result.left);
    for (auto* column : {&result.right, &result.left}) {
      std::vector<double> ideal;
      for (const auto& item : *column) {
        ideal.push_back(rowIdeal(result.dial, item.degrees));
      }
      const auto rows = placeRows(ideal, kRowHeight, result.dial.yMin, result.dial.yMax);
      for (std::size_t i = 0; i < column->size(); ++i) {
        (*column)[i].y = rows[i];
      }
    }
    auto all = result.right;
    all.insert(all.end(), result.left.begin(), result.left.end());
    result.routes = routeLeaders(result.dial, all);
    result.lanes = routeLanes(result.dial, all);
    return result;
  }

  void testLeaderLinesNeverCross() {
    struct Case {
      std::size_t count;
      std::uint32_t seed;
      double power;
    };
    const std::vector<Case> cases{
        {2, 1, 1},     {8, 2, 1},     {30, 3, 1}, {200, 4, 3},   {2500, 5, 6},
        {2500, 6, 12}, {120, 7, 0.3}, {90, 8, 8}, {5000, 9, 20}, {60, 10, 30},
    };
    std::size_t total = 0;
    std::size_t straight = 0;
    std::string modes;
    for (const auto [width, height] : {Point{1100, 560}, Point{1400, 737}, Point{2560, 1250}}) {
      for (const auto [count, seed, power] : cases) {
        const auto run = scenario(width, height, skewed(count, seed, power));
        TEST_CHECK(crossings(run.routes.paths) == 0);
        TEST_CHECK(crossings(run.lanes) == 0);
        straight += run.routes.mode == LeaderMode::Straight ? 1 : 0;
        modes += run.routes.mode == LeaderMode::Straight ? 'S' : 'L';
        ++total;
      }
    }
    TEST_CHECK(total == 30);
    TEST_CHECK(straight >= 8);
    TEST_CHECK(modes == "SLLLSSSLSLSLLLSSSLSLSLLLLLLLLL");
  }

  void testLeaderRowsKeepAngularOrderAndSpacing() {
    const auto run = scenario(1400, 737, skewed(300, 9, 5));
    TEST_CHECK(run.right.size() >= 5 && run.left.size() >= 5);
    for (const auto* column : {&run.right, &run.left}) {
      for (std::size_t i = 1; i < column->size(); ++i) {
        TEST_CHECK((*column)[i].y - (*column)[i - 1].y >= kRowHeight - 1e-9);
      }
    }
    for (std::size_t i = 1; i < run.right.size(); ++i) {
      TEST_CHECK(run.right[i].degrees > run.right[i - 1].degrees);
    }
    for (std::size_t i = 1; i < run.left.size(); ++i) {
      TEST_CHECK(run.left[i].degrees < run.left[i - 1].degrees);
    }
    TEST_CHECK(run.routes.mode == LeaderMode::Lanes);
    const auto& lane = run.lanes[0];
    TEST_CHECK(lane.size() == 15);
    TEST_NEAR(lane[1].x, 730.0903586499805, 1e-6);
    TEST_NEAR(lane[1].y, 95.18933603792948, 1e-6);
    TEST_NEAR(lane[2].x, 737.687168712609, 1e-6);
    const auto& fourth = run.routes.paths[3];
    TEST_CHECK(fourth.size() == 8);
    TEST_NEAR(fourth[0].x, 847.6791394630193, 1e-6);
    TEST_NEAR(fourth[2].y, 169.70663948592863, 1e-6);
    TEST_CHECK((fourth[7] == Point{1030, 189}));
  }

  void testLeadersStayInsideTheHost() {
    const auto run = scenario(1400, 737, skewed(500, 11, 6));
    TEST_CHECK(run.routes.paths.size() == 50);
    for (const auto& path : run.routes.paths) {
      for (const auto [x, y] : path) {
        TEST_CHECK(y >= 0 && y <= run.dial.height && x >= 0 && x <= run.dial.width);
      }
    }
  }

  void testStraightLeadersAndCrossingCount() {
    const auto dial = fullDial(1400, 737).value();
    const std::vector<LeaderItem> items{
        {.degrees = 90, .y = 409, .side = Side::Right}, {.degrees = 270, .y = 409, .side = Side::Left}
    };
    const auto routes = routeLeaders(dial, items);
    TEST_CHECK(routes.mode == LeaderMode::Straight);
    TEST_CHECK(routes.paths.size() == 2 && routes.paths[0].size() == 3);
    TEST_NEAR(routes.paths[0][0].x, 941.0, 1e-9);
    TEST_NEAR(routes.paths[0][1].x, 1004.0, 1e-9);
    TEST_CHECK((routes.paths[0][2] == Point{1030, 409}));
    TEST_CHECK((routes.paths[1][2] == Point{370, 409}));
    const std::vector<Polyline> crossed{{{0, 0}, {10, 10}}, {{0, 10}, {10, 0}}, {{20, 0}, {30, 0}}};
    TEST_CHECK(crossings(crossed) == 1);
    const std::vector<Polyline> touching{{{0, 0}, {10, 10}}, {{10, 10}, {20, 0}}};
    TEST_CHECK(crossings(touching) == 0);
    const std::vector<Polyline> shallow{{{0, 0}, {100, 10}}, {{0, 10}, {100, 0}}};
    TEST_CHECK(crossings(shallow) == 1);
    const std::vector<Polyline> nearlyParallel{{{0, 0}, {100, 1}}, {{0, 1}, {100, 0}}};
    TEST_CHECK(crossings(nearlyParallel) == 0);
    TEST_CHECK(crossings(std::vector<Polyline>{}) == 0);
    TEST_CHECK(routeLeaders(dial, std::vector<LeaderItem>{}).paths.empty());
    TEST_NEAR(rowIdeal(dial, 180), 713.0, 1e-9);
  }

} // namespace

int main() {
  testLeaderLinesNeverCross();
  testLeaderRowsKeepAngularOrderAndSpacing();
  testLeadersStayInsideTheHost();
  testStraightLeadersAndCrossingCount();
  testLaneLeadersTurnTheShortWayFromAnyAngle();
  std::println("workbench ring leaders checks passed");
  return 0;
}
