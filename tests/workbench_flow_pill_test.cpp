#include "test_check.h"
#include "workbench/flow_layout.h"
#include "workbench/scene_shapes.h"
#include "workbench_scene_fixture.h"

#include <cstddef>
#include <print>
#include <string>
#include <vector>

// An ancestor pill of the flow diagram is drawn only with its name: a name that does not fit leaves out the pill.
namespace {
  using namespace luciole::workbench;
  using luciole::test::findSceneShape;
  using luciole::test::SceneKey;

  const ColorPalette kPalette{};

  LabelFont fontOf(double advance) {
    LabelFont font;
    font.ascii.fill(advance);
    font.other = advance;
    return font;
  }

  std::size_t pillsDrawn(const Tree& tree, const FlowLayout& layout, const LabelFont& font) {
    const auto out = flowShapes(tree, {}, layout, {}, kPalette, font);
    std::size_t drawn = 0;
    for (const auto& pill : layout.stem) {
      drawn += findSceneShape(out, SceneKey{.node = pill.node, .role = SceneRole::Stem}) != nullptr ? 1U : 0U;
    }
    return drawn;
  }

  std::size_t labeledPills(const FlowLayout& layout) {
    std::size_t count = 0;
    for (const auto& pill : layout.stem) {
      count += pill.labeled && pill.textWidth > 0 ? 1U : 0U;
    }
    return count;
  }

  void aPillWhoseNameFitsIsDrawnWithIt() {
    const auto fx = luciole::test::sceneFixture();
    const auto layout = flowLayout(fx.tree, {}, 560, 280);
    TEST_CHECK(labeledPills(layout) == 2);
    TEST_CHECK(pillsDrawn(fx.tree, layout, fontOf(6)) == layout.stem.size());
    const auto out = flowShapes(fx.tree, {}, layout, {}, kPalette, fontOf(6));
    std::size_t names = 0;
    for (const auto& label : out.labels) {
      names += label.text == "user" || label.text == "home" ? 1U : 0U;
    }
    TEST_CHECK(names == 2);
  }

  void aPillWhoseNameDoesNotFitIsLeftOutWhole() {
    const auto fx = luciole::test::sceneFixture();
    const auto layout = flowLayout(fx.tree, {}, 560, 280);
    const auto out = flowShapes(fx.tree, {}, layout, {}, kPalette, fontOf(20));
    for (const auto& pill : layout.stem) {
      const bool drawn = findSceneShape(out, SceneKey{.node = pill.node, .role = SceneRole::Stem}) != nullptr;
      TEST_CHECK(drawn == (!pill.labeled || pill.textWidth <= 0));
    }
    for (const auto& label : out.labels) {
      TEST_CHECK(label.text != "user" && label.text != "home");
    }
  }

  void eachPillIsJudgedByItsOwnRoom() {
    const auto fx = luciole::test::sceneFixture();
    // The pills of "home" and "user" have 45.4 and 56 px for a name.
    const auto layout = flowLayout(fx.tree, {}, 560, 280);
    // 11 px a letter: both four-letter names fit.
    TEST_CHECK(pillsDrawn(fx.tree, layout, fontOf(11)) == layout.stem.size());
    // 13 px: "user" fits in 56, "home" does not fit 45.4 and has no cut: its pill goes, the other stays.
    TEST_CHECK(pillsDrawn(fx.tree, layout, fontOf(13)) == layout.stem.size() - 1);
  }
} // namespace

int main() {
  aPillWhoseNameFitsIsDrawnWithIt();
  aPillWhoseNameDoesNotFitIsLeftOutWhole();
  eachPillIsJudgedByItsOwnRoom();
  std::println("workbench flow pill checks passed");
  return 0;
}
