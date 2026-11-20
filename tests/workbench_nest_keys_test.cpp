#include "test_check.h"
#include "workbench/nest_keys.h"
#include "workbench_nest_fixture.h"

#include <array>
#include <cstddef>
#include <optional>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::nest;

  // A at the top left, B to its right, C below B, D far down and right.
  constexpr std::array<TreemapRect, 4> kBoxes{{
      {.x = 0, .y = 0, .w = 10, .h = 10},
      {.x = 20, .y = 0, .w = 10, .h = 10},
      {.x = 20, .y = 20, .w = 10, .h = 10},
      {.x = 40, .y = 40, .w = 10, .h = 10},
  }};

  void stopsAreTilesThenTheMoreTile() {
    auto fx = fixture();
    fx.at(12);
    const auto scene = buildNestScene(fx.tree, kSettings, 340, 380);
    const auto stops = nestStops(scene);
    std::size_t tiles = 0;
    for (const auto& piece : scene.pieces) {
      tiles += piece.kind == NestPieceKind::Tile ? 1U : 0U;
    }
    TEST_CHECK(tiles > 1 && stops.size() == tiles + 1);
    for (std::size_t i = 0; i + 1 < stops.size(); ++i) {
      TEST_CHECK(scene.pieces[stops[i]].kind == NestPieceKind::Tile);
      TEST_CHECK(i == 0 || stops[i] > stops[i - 1]);
    }
    TEST_CHECK(scene.pieces[stops.back()].kind == NestPieceKind::More);
    // A folder whose entries all fit has no "+N more" stop.
    fx.at(3);
    const auto roomy = buildNestScene(fx.tree, kSettings, 1400, 900);
    const auto all = nestStops(roomy);
    TEST_CHECK(!all.empty());
    for (const auto stop : all) {
      TEST_CHECK(roomy.pieces[stop].kind == NestPieceKind::Tile);
    }
    TEST_CHECK(nestStops(NestScene{}).empty());
  }

  void nearestFavorsWhatLiesStraightAhead() {
    TEST_CHECK(nestNearest(kBoxes, std::nullopt, 1, 0) == std::optional<std::size_t>{0});
    TEST_CHECK(nestNearest(kBoxes, 0, 1, 0) == std::optional<std::size_t>{1});
    TEST_CHECK(nestNearest(kBoxes, 0, 0, 1) == std::optional<std::size_t>{2});
    TEST_CHECK(nestNearest(kBoxes, 1, -1, 0) == std::optional<std::size_t>{0});
    TEST_CHECK(nestNearest(kBoxes, 2, 0, -1) == std::optional<std::size_t>{1});
    // Nothing ahead keeps the box; an index out of range starts at the first box.
    TEST_CHECK(nestNearest(kBoxes, 3, 1, 0) == std::optional<std::size_t>{3});
    TEST_CHECK(nestNearest(kBoxes, 9, 1, 0) == std::optional<std::size_t>{0});
    TEST_CHECK(nestNearest(kBoxes, 1, 0, 0) == std::optional<std::size_t>{1});
    TEST_CHECK(nestNearest(kBoxes, 1, kNan, 0) == std::optional<std::size_t>{1});
    TEST_CHECK(!nestNearest({}, std::nullopt, 1, 0).has_value());
  }
} // namespace

int main() {
  stopsAreTilesThenTheMoreTile();
  nearestFavorsWhatLiesStraightAhead();
  std::println("workbench nest keys checks passed");
  return 0;
}
