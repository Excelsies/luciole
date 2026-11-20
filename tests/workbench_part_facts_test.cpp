#include "test_check.h"
#include "workbench/flame_scene.h"
#include "workbench/flow_layout.h"
#include "workbench/iris_facts.h"
#include "workbench/ladder_plan.h"
#include "workbench/nest_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"
#include "workbench_scene_fixture.h"

#include <array>
#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// A fact is told once: when the measure shown is the second fact, the line shows it a single time.
namespace {
  using namespace luciole::workbench;
  using luciole::WorkbenchMeasure;

  const ColorPalette kPalette{};
  constexpr std::array<WorkbenchMeasure, 6> kMeasures{
      WorkbenchMeasure::Bytes,    WorkbenchMeasure::Files,  WorkbenchMeasure::Items,
      WorkbenchMeasure::Modified, WorkbenchMeasure::Visits, WorkbenchMeasure::Equal,
  };

  // The facts of a line, split at the middle dots.
  std::vector<std::string> factsOf(std::string_view line) {
    std::vector<std::string> facts;
    std::string current;
    std::size_t i = 0;
    while (i < line.size()) {
      if (line.substr(i, 2) == "\xC2\xB7") {
        facts.push_back(current);
        current.clear();
        i += 2;
        continue;
      }
      if (line[i] != ' ') {
        current += line[i];
      }
      ++i;
    }
    facts.push_back(current);
    return facts;
  }

  bool repeatsAFact(std::string_view line) {
    const auto facts = factsOf(line);
    for (std::size_t i = 0; i < facts.size(); ++i) {
      for (std::size_t j = i + 1; j < facts.size(); ++j) {
        if (!facts[i].empty() && facts[i] == facts[j]) {
          return true;
        }
      }
    }
    return false;
  }

  void theCheckSeesARepeat() {
    TEST_CHECK(repeatsAFact("9 items  ·  9 items"));
    TEST_CHECK(repeatsAFact("a \xC2\xB7 b \xC2\xB7 a"));
    TEST_CHECK(!repeatsAFact("9 items  ·  3.0 KiB"));
    TEST_CHECK(!repeatsAFact("name"));
  }

  void checkLabels(const SceneShapes& out, std::string_view part, WorkbenchMeasure by) {
    for (const auto& label : out.labels) {
      if (repeatsAFact(label.text)) {
        std::println("{} by {}: \"{}\"", part, static_cast<int>(by), label.text);
      }
      TEST_CHECK(!repeatsAFact(label.text));
    }
  }

  void nestTilesAndCardTellEachFactOnce() {
    auto fx = luciole::test::sceneFixture();
    for (const auto by : kMeasures) {
      LayoutSettings settings;
      settings.sizeBy = by;
      const auto scene = buildNestScene(fx.tree, settings, 900, 600, kPalette);
      checkLabels(nestShapes(fx.tree, scene, {}, kPalette), "nest", by);
      for (const auto& piece : scene.pieces) {
        if (piece.kind != NestPieceKind::Tile || !piece.folder) {
          continue;
        }
        if (const auto* label = std::get_if<NestTileLabel>(&piece.label)) {
          TEST_CHECK(!repeatsAFact(label->detail));
        }
        if (const auto peek = buildNestPeek(fx.tree, settings, scene, &piece - scene.pieces.data(), kPalette)) {
          TEST_CHECK(!repeatsAFact(peek->facts));
        }
      }
    }
  }

  void aFolderTileUnderItemsShowsItsCountOnce() {
    auto fx = luciole::test::sceneFixture();
    LayoutSettings settings;
    settings.sizeBy = WorkbenchMeasure::Items;
    const auto id = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto label = nestTileLabel(fx.tree, settings, id, {.x = 0, .y = 0, .w = 260, .h = 120});
    TEST_CHECK(label.has_value() && label->twoLines);
    TEST_CHECK(label->detail == "3 items");
    settings.sizeBy = WorkbenchMeasure::Bytes;
    const auto bytes = nestTileLabel(fx.tree, settings, id, {.x = 0, .y = 0, .w = 260, .h = 120});
    TEST_CHECK(bytes.has_value() && bytes->detail.ends_with("3 items") && bytes->detail != "3 items");
  }

  void flameBarsTellEachFactOnce() {
    auto fx = luciole::test::sceneFixture();
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    for (const auto by : kMeasures) {
      LayoutSettings settings;
      settings.sizeBy = by;
      const auto scene = flameScene(
          fx.tree, settings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font}
      );
      checkLabels(flameShapes(fx.tree, settings, scene, {}, kPalette), "flame", by);
    }
  }

  void ladderAndFlowTellEachFactOnce() {
    auto fx = luciole::test::sceneFixture();
    LabelFont font;
    font.ascii.fill(6);
    font.other = 6;
    for (const auto by : kMeasures) {
      LayoutSettings settings;
      settings.sizeBy = by;
      const auto ladder = ladderScene(fx.tree, settings, 900, 160);
      checkLabels(ladderShapes(fx.tree, settings, ladder, {}, kPalette), "ladder", by);
      const auto flow = flowLayout(fx.tree, settings, 900, 300);
      checkLabels(flowShapes(fx.tree, settings, flow, {}, kPalette, font), "flow", by);
    }
  }

  void irisReadoutTellsEachFactOnce() {
    const auto tree = luciole::test::iris::folderX();
    for (const auto by : kMeasures) {
      auto input = luciole::test::iris::hostInput(340, 534, true);
      input.settings.sizeBy = by;
      const auto scene = luciole::workbench::iris::buildScene(tree, input).value();
      const auto text =
          luciole::workbench::iris::readoutText(tree, scene, {}, luciole::workbench::iris::CursorStop::Marks);
      TEST_CHECK(!repeatsAFact(text));
    }
  }
} // namespace

int main() {
  theCheckSeesARepeat();
  nestTilesAndCardTellEachFactOnce();
  aFolderTileUnderItemsShowsItsCountOnce();
  flameBarsTellEachFactOnce();
  ladderAndFlowTellEachFactOnce();
  irisReadoutTellsEachFactOnce();
  std::println("workbench part facts checks passed");
  return 0;
}
