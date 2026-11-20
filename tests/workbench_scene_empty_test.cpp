#include "test_check.h"
#include "workbench/flame_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <string_view>
#include <vector>

// The flame graph of a folder with no folders ahead.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;
  using luciole::test::WorkbenchRow;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  FlameScene flame(const Tree& tree) {
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    return flameScene(tree, kSettings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font});
  }

  // A tree top > here whose folder `here` holds `kids`, entered at here.
  Tree enteredHere(std::vector<WorkbenchRow> kids) {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 2e5, .items = 1},
        {.parent = 0, .name = "here", .folder = true, .bytes = 1e5, .items = static_cast<double>(kids.size())},
    };
    rows.insert(rows.end(), kids.begin(), kids.end());
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  // With no folder ahead the strata ahead hold a dashed outline; a folder of files says where its files are, and an
  // empty one leaves the words to the view's note.
  void testFlameEmptyState() {
    const auto outlineOf = [](const Tree& tree, const SceneShapes& out) -> const Shape* {
      for (const auto* shape : drawn(out, {tree.root, SceneRole::Detail})) {
        if (shape->pattern == ShapePattern::DashedOutline) {
          return shape;
        }
      }
      return nullptr;
    };
    const auto files = enteredHere({{.parent = 1, .name = "a.txt", .bytes = 5e4}, {.parent = 1, .name = "b.txt"}});
    const auto scene = flame(files);
    TEST_CHECK(scene.empty && scene.aheadBox.has_value());
    const auto out = flameShapes(files, kSettings, scene, {}, kPalette);
    const auto* outline = outlineOf(files, out);
    TEST_CHECK(outline != nullptr && outline->strokeWidth == 1 && outline->fill.a == 0 && !outline->hittable);
    TEST_CHECK(sameRgb(outline->stroke, kPalette.outlineVariant));
    TEST_CHECK(sameRect(
        outline->rect, scene.aheadBox->x + 0.5, scene.aheadBox->y + 0.5, scene.aheadBox->width - 1,
        scene.aheadBox->height - 1
    ));
    const auto line = [&](std::string_view text) {
      return std::ranges::find_if(out.labels, [&](const ShapeLabel& l) { return l.text == text; });
    };
    const auto title = line("No folders inside");
    const auto hint = line("The files are in the list below");
    TEST_CHECK(title != out.labels.end() && hint != out.labels.end());
    TEST_CHECK(title->align == LabelAlign::Center && title->fontSize == 13 && hint->fontSize == 12);
    TEST_CHECK(hint->y > title->y && hint->color.a < 255 && sameRgb(title->color, kPalette.onSurfaceVariant));
    TEST_NEAR(title->x + title->width / 2, scene.aheadBox->x + scene.aheadBox->width / 2, 1e-4);

    const auto bare = enteredHere({});
    const auto bareOut = flameShapes(bare, kSettings, flame(bare), {}, kPalette);
    TEST_CHECK(outlineOf(bare, bareOut) != nullptr);
    TEST_CHECK(std::ranges::none_of(bareOut.labels, [](const ShapeLabel& l) { return l.align == LabelAlign::Center; }));

    const auto folders = enteredHere({{.parent = 1, .name = "sub", .folder = true, .bytes = 5e4}});
    const auto folderScene = flame(folders);
    TEST_CHECK(!folderScene.empty && folderScene.aheadBox.has_value());
    TEST_CHECK(outlineOf(folders, flameShapes(folders, kSettings, folderScene, {}, kPalette)) == nullptr);
  }
} // namespace

int main() {
  testFlameEmptyState();
  return 0;
}
