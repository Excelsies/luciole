#include "test_check.h"
#include "workbench/iris_facts.h"
#include "workbench/iris_hit.h"
#include "workbench/scene_shapes.h"
#include "workbench_iris_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {
  using namespace luciole::test::iris;
  using luciole::test::firstDrawn;
  using luciole::test::hitTarget;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;

  const ColorPalette kPalette{};

  ShapeMarks hovering(NodeId node, SceneRole role) {
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = node, .role = shapeRole(role)};
    return marks;
  }

  const Shape* chipShape(const SceneShapes& out, std::size_t index) {
    const auto found = std::ranges::find_if(out.shapes, [index](const Shape& s) { return s.id == chipId(index); });
    return found == out.shapes.end() ? nullptr : &*found;
  }

  const ShapeLabel* labelWith(const SceneShapes& out, const std::string& text) {
    const auto found = std::ranges::find_if(out.labels, [&text](const ShapeLabel& l) { return l.text == text; });
    return found == out.labels.end() ? nullptr : &*found;
  }

  // Ids of folderX: 1 a, 5 x, 7 deep (a folder holding g.c), 8 f.log.
  Scene compactX(std::vector<NodeId> trail = {}) {
    auto input = hostInput(340, 534, true);
    input.trail = std::move(trail);
    return buildScene(folderX(), input).value();
  }

  // Each hub line is a label centered on its line; the name and the total are semibold.
  void hubLinesAreLabels() {
    const auto tree = folderX();
    const auto scene = compactX({7});
    const auto out = irisShapes(tree, scene, {}, kPalette);
    TEST_CHECK(scene.hub.lines.size() == 3);
    for (const auto& line : scene.hub.lines) {
      const auto* label = labelWith(out, line.text);
      TEST_CHECK(label != nullptr);
      TEST_NEAR(label->x + label->width / 2, line.box.x + line.box.width / 2, 1e-3);
      TEST_CHECK(label->width >= line.box.width - 1e-3F && label->align == LabelAlign::Center);
      const bool bold = line.ink == HubInk::Name || line.ink == HubInk::Total;
      TEST_CHECK(label->weight == (bold ? LabelWeight::Semibold : LabelWeight::Regular));
      const bool count = line.ink == HubInk::Total || line.ink == HubInk::Items;
      TEST_CHECK(label->fit == (count ? LabelFit::Whole : LabelFit::Shorten));
      TEST_NEAR(label->fontSize, line.fontSize, 1e-4);
      TEST_NEAR(label->y + label->height / 2, line.box.y + line.box.height / 2, 1e-3);
      const auto ink = line.ink == HubInk::Total ? kPalette.primary
          : line.ink == HubInk::Items            ? kPalette.onSurfaceVariant
                                                 : kPalette.onSurface;
      TEST_CHECK(sameRgb(label->color, ink));
    }
  }

  void chipsAreButtonsAboveTheHub() {
    const auto tree = folderX();
    const auto scene = compactX({7});
    const auto& chip = scene.hub.chips.at(0);
    const auto out = irisShapes(tree, scene, {}, kPalette);
    const auto* shape = chipShape(out, 0);
    TEST_CHECK(shape != nullptr && shape->kind == ShapeKind::RoundedRect);
    TEST_CHECK(sameRect(shape->rect, chip.box.x, chip.box.y, chip.box.width, chip.box.height));
    TEST_CHECK(sameRgb(shape->fill, kPalette.surfaceContainerHigh) && shape->fill.a == 255);
    TEST_CHECK(sameRgb(shape->stroke, kPalette.outlineVariant) && shape->strokeWidth == 1);
    // The chip lies over the hub and takes the hit there.
    const ShapePoint middle{
        .x = static_cast<float>(chip.box.x + chip.box.width / 2),
        .y = static_cast<float>(chip.box.y + chip.box.height / 2)
    };
    const auto hit = hitShape(out.shapes, middle, 0);
    TEST_CHECK(hit && out.shapes[*hit].id == chipId(0));
    const auto* label = labelWith(out, chip.text);
    TEST_CHECK(label != nullptr);
    TEST_NEAR(label->x, chip.box.x + kChipPad, 1e-3);
    TEST_NEAR(label->fontSize, chip.fontSize, 1e-4);
  }

  // Only the chip under the pointer lights up.
  void chipsLightUpOneAtATime() {
    const auto tree = folderX();
    auto input = fullInput();
    input.trail = {7, 9};
    const auto scene = buildScene(tree, input).value();
    TEST_CHECK(scene.hub.chips.size() == 2);
    ShapeMarks marks;
    marks.hovered = chipId(1);
    const auto hovered = irisShapes(tree, scene, marks, kPalette);
    const auto* hot = chipShape(hovered, 1);
    TEST_CHECK(sameRgb(hot->fill, kPalette.surfaceContainerHighest) && sameRgb(hot->stroke, kPalette.secondary));
    const auto* quiet = chipShape(hovered, 0);
    TEST_CHECK(sameRgb(quiet->fill, kPalette.surfaceContainerHigh) && sameRgb(quiet->stroke, kPalette.outlineVariant));
  }

  // A folder merged into "+N more" that the trail offers is drawn by no shape of its own: its chip names it only by
  // its place among the chips, so the folder still counts as one the merged piece holds.
  void chipsDrawNoPieceOfTheirFolder() {
    const auto files = fileFolder("f");
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 1.9e6, .items = 61}
    };
    for (std::size_t i = 0; i < files.names.size(); ++i) {
      rows.push_back({.parent = 0, .name = files.names[i], .bytes = (60.0 - static_cast<double>(i)) * 1000});
    }
    rows.push_back({.parent = 0, .name = "small", .folder = true, .bytes = 10, .items = 1});
    const auto tree = luciole::test::workbenchTree(rows, 0);
    constexpr NodeId kSmall = 61;
    auto input = fullInput();
    input.trail = {kSmall};
    const auto scene = buildScene(tree, input).value();
    const auto more =
        std::ranges::find_if(scene.main.sectors, [](const Sector& s) { return s.kind == SectorKind::More; });
    TEST_CHECK(more != scene.main.sectors.end() && std::ranges::contains(more->merged, kSmall));
    TEST_CHECK(scene.hub.chips.size() == 1 && scene.hub.chips[0].node == kSmall);
    const auto out = irisShapes(tree, scene, {}, kPalette);
    TEST_CHECK(chipShape(out, 0) != nullptr);
    TEST_CHECK(std::ranges::none_of(out.shapes, [](const Shape& shape) { return shape.id.node == kSmall; }));
  }

  // A stack row's background is as wide as its text and shows only under the pointer.
  void stackRowsLightUpUnderThePointer() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    const auto& row = scene.stack.rows.back();
    TEST_CHECK(row.node == 1);
    const auto out = irisShapes(tree, scene, {}, kPalette);
    const auto* shape = hitTarget(out, {1, SceneRole::Stack});
    TEST_CHECK(shape != nullptr && sameRect(shape->rect, row.box.x, row.box.y, row.box.width, row.box.height));
    TEST_CHECK(shape->fill.a == 0);
    const auto* label = labelWith(out, "a");
    TEST_CHECK(label != nullptr && label->align == LabelAlign::Start);
    TEST_NEAR(label->x, row.box.x + kStackPad, 1e-3);
    TEST_CHECK(sameRgb(label->color, kPalette.onSurfaceVariant));
    const auto hovered = irisShapes(tree, scene, hovering(1, SceneRole::Stack), kPalette);
    const auto* hot = hitTarget(hovered, {1, SceneRole::Stack});
    TEST_CHECK(hot->fill.a == 255 && sameRgb(hot->fill, kPalette.surfaceContainerHigh));
    TEST_CHECK(sameRgb(labelWith(hovered, "a")->color, kPalette.onSurface));
  }

  void compactDialsHaveAReadout() {
    const auto tree = folderX();
    const auto scene = compactX();
    const auto marks = hovering(7, SceneRole::Piece);
    const auto out = irisShapes(tree, scene, marks, kPalette);
    const auto line = readoutLine(tree, scene, marks, CursorStop::Marks);
    TEST_CHECK(line.text == "deep");
    // The name may be cut; its size and count are a whole trail at the right end of the line.
    const auto* label = labelWith(out, line.text);
    TEST_CHECK(label != nullptr && label->align == LabelAlign::Start && label->fit == LabelFit::Shorten);
    TEST_CHECK(label->trail.has_value() && label->trail->text == line.facts);
    TEST_CHECK(label->trail->place == TrailPlace::End && label->trail->fontSize == 11);
    const auto& box = scene.readout->box;
    TEST_NEAR(label->x, box.x, 1e-3);
    TEST_NEAR(label->width, box.width, 1e-3);
    TEST_NEAR(label->fontSize, 11, 1e-4);
    TEST_NEAR(label->y + label->height / 2, box.y + box.height / 2, 1e-3);
    TEST_CHECK(sameRgb(label->color, kPalette.onSurfaceVariant));
    const auto full = buildScene(tree, fullInput()).value();
    const auto fullOut = irisShapes(tree, full, marks, kPalette);
    TEST_CHECK(std::ranges::none_of(fullOut.labels, [](const ShapeLabel& l) { return l.trail.has_value(); }));
  }

  // A hovered arc is filled with its accent; its name switches to the ink that reads on that accent.
  void hoveredArcNamesReadOnTheirAccent() {
    const auto tree = folderX();
    const auto scene = compactX();
    const auto found = std::ranges::find_if(scene.arcLabels, [&scene](const ArcLabel& arc) {
      return scene.main.sectors[arc.sector].node == 7;
    });
    TEST_CHECK(found != scene.arcLabels.end());
    const auto& sector = scene.main.sectors[found->sector];
    const auto plainOut = irisShapes(tree, scene, {}, kPalette);
    const auto* plain = labelWith(plainOut, found->text);
    TEST_CHECK(plain != nullptr && sameRgb(plain->color, found->ink));
    const auto hovered = irisShapes(tree, scene, hovering(7, SceneRole::Piece), kPalette);
    const auto ink = inkFor(sector.color.accent, kPalette);
    // The readout line shows the same name, with its facts as a trail; the arc's label has none.
    const auto arc = std::ranges::find_if(hovered.labels, [&](const ShapeLabel& l) {
      return l.text == found->text && !l.trail.has_value();
    });
    TEST_CHECK(arc != hovered.labels.end());
    TEST_CHECK(ink != found->ink && sameRgb(arc->color, ink));
  }

  // Shapes of the inner band: sectors between rIn + 4 and rIn + 14 that take no hits.
  std::size_t bandPieces(const SceneShapes& out, const Scene& scene) {
    return static_cast<std::size_t>(std::ranges::count_if(out.shapes, [&scene](const Shape& shape) {
      return shape.kind == ShapeKind::Sector
          && !shape.hittable
          && std::abs(shape.sector.r0 - (scene.dial.rIn + 4)) < 1e-3
          && std::abs(shape.sector.r1 - (scene.dial.rIn + 14)) < 1e-3;
    }));
  }

  void innerBandShowsTheHoveredOrCursorFolder() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    TEST_CHECK(bandPieces(irisShapes(tree, scene, {}, kPalette), scene) == 0);
    const auto hovered = irisShapes(tree, scene, hovering(7, SceneRole::Piece), kPalette);
    TEST_CHECK(bandPieces(hovered, scene) == 1);
    TEST_CHECK(firstDrawn(hovered, {9, SceneRole::Detail}) != nullptr); // g.c, the folder's one entry
    TEST_CHECK(bandPieces(irisShapes(tree, scene, hovering(7, SceneRole::Row), kPalette), scene) == 1);
    TEST_CHECK(bandPieces(irisShapes(tree, scene, hovering(8, SceneRole::Piece), kPalette), scene) == 0);
    const ShapeMarks cursor{.hovered = {}, .linked = kNoNode, .cursor = 7};
    TEST_CHECK(bandPieces(irisShapes(tree, scene, cursor, kPalette), scene) == 1);
    // A hovered context piece is no entry of the folder: the cursor's folder shows.
    auto context = hovering(1, SceneRole::Piece);
    context.cursor = 7;
    TEST_CHECK(bandPieces(irisShapes(tree, scene, context, kPalette), scene) == 1);
    // A linked entry of the folder counts as hovered.
    TEST_CHECK(bandPieces(irisShapes(tree, scene, {.hovered = {}, .linked = 7, .cursor = 8}, kPalette), scene) == 1);
    TEST_CHECK(bandPieces(irisShapes(tree, scene, {.hovered = {}, .linked = 8, .cursor = 7}, kPalette), scene) == 0);
  }

  void theCursorCanRestOnMore() {
    const auto files = fileFolder("f");
    const auto scene = buildScene(files.tree, fullInput()).value();
    const NodeId first = files.tree.children[0];
    const ShapeMarks marks{.hovered = {}, .linked = kNoNode, .cursor = first};
    const auto plain = irisShapes(files.tree, scene, marks, kPalette);
    TEST_CHECK(firstDrawn(plain, {first, SceneRole::Mark}) != nullptr);
    TEST_CHECK(firstDrawn(plain, {0, SceneRole::Mark}) == nullptr);
    const auto more = irisShapes(files.tree, scene, marks, kPalette, CursorStop::More);
    TEST_CHECK(firstDrawn(more, {first, SceneRole::Mark}) == nullptr);
    const auto* mark = firstDrawn(more, {0, SceneRole::Mark});
    TEST_CHECK(mark != nullptr && sameRgb(mark->stroke, kPalette.primary));
    // The "+N more" row shows the cursor's outline.
    const auto* row = firstDrawn(more, {0, SceneRole::Row});
    TEST_CHECK(row != nullptr && row->strokeWidth > 0 && sameRgb(row->stroke, kPalette.primary));
    TEST_CHECK(firstDrawn(plain, {0, SceneRole::Row})->strokeWidth == 0);
    // Its count shows whole or not at all.
    const auto count = std::ranges::find_if(plain.labels, [](const ShapeLabel& label) {
      return label.text.starts_with('+') && label.text.ends_with(" more");
    });
    TEST_CHECK(count != plain.labels.end() && count->fit == LabelFit::Whole);
  }

  // Full rows carry their entry's icon, 18 px in the accent, between the dot and the name; compact rows none.
  void rowLabelsCarryTheirIcon() {
    const auto tree = folderX();
    const auto scene = buildScene(tree, fullInput()).value();
    const auto out = irisShapes(tree, scene, {}, kPalette);
    std::size_t checked = 0;
    for (const auto* column : {&scene.rows.right, &scene.rows.left}) {
      for (const auto& row : *column) {
        const auto& sector = scene.main.sectors[row.sector];
        const auto* label = labelWith(out, row.name);
        TEST_CHECK(label != nullptr);
        // The measure beside the name shows whole or not at all.
        const auto* metric = labelWith(out, row.metric);
        TEST_CHECK(row.metric.empty() || (metric != nullptr && metric->fit == LabelFit::Whole));
        if (sector.kind != SectorKind::Entry) {
          TEST_CHECK(label->icon.size == 0);
          continue;
        }
        TEST_CHECK(label->icon.size == 18);
        TEST_CHECK(label->icon.category == static_cast<FileCategory>(tree.nodes[sector.node].category));
        TEST_CHECK(sameRgb(label->icon.color, sector.color.accent));
        TEST_NEAR(label->icon.top, (label->height - 18) / 2, 1e-4);
        const float expected = row.side == ring::Side::Right ? -26.0F : label->width + 8;
        TEST_NEAR(label->icon.left, expected, 1e-3);
        ++checked;
      }
    }
    TEST_CHECK(checked == 2 && !scene.rows.left.empty());
    auto legend = hostInput(860, 400, true);
    const auto compact = buildScene(tree, legend).value();
    TEST_CHECK(compact.dial.legend);
    const auto compactOut = irisShapes(tree, compact, {}, kPalette);
    TEST_CHECK(labelWith(compactOut, "deep") != nullptr && labelWith(compactOut, "deep")->icon.size == 0);
  }
} // namespace

int main() {
  hubLinesAreLabels();
  chipsAreButtonsAboveTheHub();
  chipsLightUpOneAtATime();
  chipsDrawNoPieceOfTheirFolder();
  stackRowsLightUpUnderThePointer();
  compactDialsHaveAReadout();
  hoveredArcNamesReadOnTheirAccent();
  innerBandShowsTheHoveredOrCursorFolder();
  theCursorCanRestOnMore();
  rowLabelsCarryTheirIcon();
  return 0;
}
