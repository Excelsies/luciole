#include "test_check.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The drawn look of the flame graph, the ladder and the flow diagram.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::findSceneShape;
  using luciole::test::firstDrawn;
  using luciole::test::hitTarget;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;
  using luciole::test::SceneKey;
  using luciole::test::sceneKeyOf;
  using luciole::test::WorkbenchRow;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  // top > here; here holds one large folder and 60 small ones, each small one a folder of one file.
  Tree crowdedTree() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 9e6, .items = 2},
        {.parent = 0, .name = "here", .folder = true, .bytes = 8e6, .items = 61},
        {.parent = 0, .name = "other", .folder = true, .bytes = 1e6, .items = 0},
        {.parent = 1, .name = "big", .folder = true, .bytes = 5e6, .items = 0},
    };
    std::vector<std::string> names(60);
    for (std::size_t i = 0; i < names.size(); ++i) {
      names[i] = std::format("s{:02}", i);
      rows.push_back({.parent = 1, .name = names[i], .folder = true, .bytes = 1e4, .items = 0});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  FlameScene flame(const Tree& tree) {
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    return flameScene(tree, kSettings, 1000, 266, {.firstAhead = font, .ahead = font, .ancestor = font, .value = font});
  }

  // Whether an opaque shape that takes no hits, drawn after `piece`, covers part of the box more than `least` px
  // across.
  bool coveredAfter(const SceneShapes& out, const Shape* piece, ShapeRect box, double least) {
    const auto from = static_cast<std::size_t>(piece - out.shapes.data()) + 1;
    for (std::size_t i = from; i < out.shapes.size(); ++i) {
      const auto& r = out.shapes[i].rect;
      const double width = std::min(r.x + r.width, box.x + box.width) - std::max(r.x, box.x);
      const double height = std::min(r.y + r.height, box.y + box.height) - std::max(r.y, box.y);
      if (!out.shapes[i].hittable && out.shapes[i].fill.a == 255 && width > least && height > least) {
        return true;
      }
    }
    return false;
  }

  // The band a piece's own outline draws in, as its four sides.
  std::vector<ShapeRect> strokeBand(const Shape& piece) {
    const auto& r = piece.rect;
    const float w = piece.strokeWidth;
    return {
        {.x = r.x, .y = r.y, .width = w, .height = r.height},
        {.x = r.x + r.width - w, .y = r.y, .width = w, .height = r.height},
        {.x = r.x, .y = r.y, .width = r.width, .height = w},
        {.x = r.x, .y = r.y + r.height - w, .width = r.width, .height = w},
    };
  }

  const FlameSceneRow& firstAhead(const FlameScene& scene) {
    return *std::ranges::find_if(scene.rows, [](const FlameSceneRow& row) {
      return row.row.kind == FlameRowKind::Ahead && row.row.level == 1;
    });
  }

  void testFlameBarPinsAndMore() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = flame(fx.tree);
    const auto out = flameShapes(fx.tree, kSettings, scene, {}, kPalette);
    const auto* bar = findSceneShape(out, {fx.work, SceneRole::Path});
    TEST_CHECK(bar != nullptr && sameRgb(bar->fill, kPalette.surfaceContainerHigh) && bar->strokeWidth == 0);
    TEST_NEAR(bar->rect.radius, 4.0F, 1e-6);
    // The bar names the folder after its icon's room and gives its measure and items at the right.
    const auto name = std::ranges::find_if(out.labels, [&](const ShapeLabel& l) {
      return l.node == fx.work && l.text == "work" && l.x > bar->rect.x + 30;
    });
    TEST_CHECK(name != out.labels.end());
    // The folder's icon sits in that room, 10 px in from the bar's left end and centered on its height.
    TEST_CHECK(name->icon.category == FileCategory::Folder && name->icon.size == 16);
    TEST_NEAR(name->x + name->icon.left, bar->rect.x + 10, 1e-4);
    TEST_NEAR(name->y + name->icon.top + 8, bar->rect.y + bar->rect.height / 2, 1e-4);
    TEST_CHECK(sameRgb(name->icon.color, nodeColor(fx.tree, fx.work, kSettings, kPalette).accent));
    TEST_CHECK(std::ranges::any_of(out.labels, [&](const ShapeLabel& l) {
      return l.node == fx.work && l.text.ends_with("24 items") && l.align == LabelAlign::End;
    }));
    std::size_t pinned = 0;
    for (const auto& row : scene.rows) {
      for (const auto& item : row.items) {
        if (item.pinned && row.row.h >= 10) {
          const auto* shape = findSceneShape(out, {item.node, SceneRole::Piece});
          TEST_CHECK(sameRgb(shape->stroke, kPalette.primary) && shape->strokeWidth == 1.5F);
          ++pinned;
        }
      }
    }
    TEST_CHECK(pinned > 0);
    // Merged folders: a quiet fill with a vertical grain, no dashes.
    const auto crowded = crowdedTree();
    const auto many = flame(crowded);
    const auto shapes = flameShapes(crowded, kSettings, many, {}, kPalette);
    const auto& row = firstAhead(many);
    const auto more = std::ranges::find_if(row.items, [](const FlameItem& item) { return item.more; });
    TEST_CHECK(more != row.items.end());
    const auto* piece = findSceneShape(shapes, {more->owner, SceneRole::More});
    TEST_CHECK(sameRgb(piece->fill, kPalette.surfaceContainerHighest) && piece->pattern == ShapePattern::Solid);
    const auto grain = drawn(shapes, {more->owner, SceneRole::Detail});
    TEST_CHECK(grain.size() >= 2);
    for (const auto* line : grain) {
      TEST_CHECK(line->kind == ShapeKind::Line && sameRgb(line->fill, kPalette.surfaceContainerLow));
      TEST_CHECK(line->line.from.x > piece->rect.x && line->line.from.x < piece->rect.x + piece->rect.width);
    }
  }

  void testFlameFocus() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = flame(fx.tree);
    const auto plain = flameShapes(fx.tree, kSettings, scene, {}, kPalette);
    const auto& first = firstAhead(scene);
    const NodeId focus = first.items.front().node;
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = focus, .role = shapeRole(SceneRole::Piece)};
    const auto hot = flameShapes(fx.tree, kSettings, scene, marks, kPalette);
    const auto color = nodeColor(fx.tree, focus, kSettings, kPalette);
    const auto* lit = findSceneShape(hot, {focus, SceneRole::Piece});
    // The hovered folder turns to its accent and gets an onSurface outline.
    TEST_CHECK(
        sameRgb(lit->fill, color.accent) && sameRgb(lit->stroke, kPalette.onSurface) && lit->strokeWidth == 1.5F
    );
    std::size_t sub = 0;
    std::size_t dim = 0;
    for (const auto& row : scene.rows) {
      if (row.row.kind != FlameRowKind::Ahead || row.row.level == 1) {
        continue;
      }
      for (const auto& item : row.items) {
        if (item.more) {
          continue;
        }
        const auto* before = findSceneShape(plain, {item.node, SceneRole::Piece});
        const auto* after = findSceneShape(hot, {item.node, SceneRole::Piece});
        if (flameFirstLevel(fx.tree, item.node, fx.work) == focus) {
          // Its subtree moves toward the accent, at full strength.
          TEST_CHECK(after->fill != before->fill && after->opacity == 1);
          ++sub;
        } else {
          TEST_CHECK(after->opacity < 0.5F && after->fill == before->fill);
          ++dim;
        }
      }
    }
    TEST_CHECK(sub > 0 && dim > 0);
    const auto cursor =
        flameShapes(fx.tree, kSettings, scene, {.hovered = {}, .linked = kNoNode, .cursor = focus}, kPalette);
    // The cursor is a full strength primary ring on the piece, over any dimming.
    const auto* ring = firstDrawn(cursor, {focus, SceneRole::Mark});
    const auto* piece = findSceneShape(cursor, {focus, SceneRole::Piece});
    TEST_CHECK(ring != nullptr && sameRgb(ring->stroke, kPalette.primary) && ring->strokeWidth == 2);
    TEST_CHECK(
        ring->opacity == 1 && sameRect(ring->rect, piece->rect.x, piece->rect.y, piece->rect.width, piece->rect.height)
    );
  }

  // The outline of an on-path piece lies just outside it in primary: straight sides as lines, and on the side where
  // the piece starts or ends its bar two quarter rings around its rounded corners.
  void pathOutlineFollowsTheBar(const SceneShapes& out, const Shape& piece, double fade, bool first, bool last) {
    const auto parts = drawn(out, {piece.id.node, SceneRole::Mark});
    TEST_CHECK(!parts.empty());
    const auto& r = piece.rect;
    std::size_t leftArcs = 0;
    std::size_t rightArcs = 0;
    for (const auto* part : parts) {
      TEST_CHECK(fade < 1 || sameRgb(part->fill, kPalette.primary));
      if (part->kind == ShapeKind::Sector) {
        TEST_NEAR(part->sector.r0, std::min({r.radius, r.width / 2, r.height / 2}), 1e-4);
        TEST_CHECK(part->sector.a1 - part->sector.a0 == 90);
        const bool leftSide = part->sector.center.x < r.x + r.width / 2;
        leftArcs += leftSide ? 1 : 0;
        rightArcs += leftSide ? 0 : 1;
        continue;
      }
      TEST_CHECK(part->kind == ShapeKind::Line);
      const auto& line = part->line;
      const float half = line.width / 2;
      // Each side's middle lies half the outline's width outside the piece.
      const bool horizontal = line.from.y == line.to.y;
      if (horizontal) {
        TEST_CHECK(
            std::abs(line.from.y - (r.y - half)) < 1e-4 || std::abs(line.from.y - (r.y + r.height + half)) < 1e-4
        );
      } else {
        TEST_CHECK(
            std::abs(line.from.x - (r.x - half)) < 1e-4 || std::abs(line.from.x - (r.x + r.width + half)) < 1e-4
        );
      }
    }
    TEST_CHECK(leftArcs == (first ? 2U : 0U));
    TEST_CHECK(rightArcs == (last ? 2U : 0U));
  }

  void testLadderSegments() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = ladderScene(fx.tree, kSettings, 900, 160);
    const auto out = ladderShapes(fx.tree, kSettings, scene, {}, kPalette);
    std::size_t seams = 0;
    for (const auto& row : scene.rows) {
      const std::size_t count = row.pieces.size() + (row.rest ? 1 : 0);
      for (std::size_t i = 0; i < row.pieces.size(); ++i) {
        const auto& piece = row.pieces[i];
        const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
        TEST_CHECK(shape != nullptr && sameRect(shape->rect, piece.x, row.y, piece.w, row.h));
        TEST_CHECK(shape->strokeWidth == 0);
        // The segment is nearer its accent than the dark fill other parts use.
        const auto color = nodeColor(fx.tree, piece.node, kSettings, kPalette);
        const auto distance = [&](Rgb rgb) {
          return std::abs(shape->fill.r - rgb.r) + std::abs(shape->fill.g - rgb.g) + std::abs(shape->fill.b - rgb.b);
        };
        TEST_CHECK(row.fade < 1 || distance(color.accent) < distance(color.fill));
        // Bar ends are rounded; the inner corners stay square.
        const bool end = i == 0 || i + 1 == count;
        TEST_CHECK((shape->rect.radius > 0) == end);
        if (i == 0 && count > 1) {
          const ShapePoint corner{.x = shape->rect.x + shape->rect.width - 2.5F, .y = shape->rect.y + 0.3F};
          TEST_CHECK(std::ranges::any_of(out.shapes, [&](const Shape& s) {
            return s.fill == shape->fill && shapeContains(s, corner, 0);
          }));
        }
        // A line of surface color parts it from the next one.
        if (i + 1 < count) {
          const auto edge = static_cast<float>(piece.x + piece.w);
          const bool seam = std::ranges::any_of(drawn(out, {piece.node, SceneRole::Detail}), [&](const Shape* s) {
            const bool color = row.fade < 1 || sameRgb(s->fill, kPalette.surface);
            return color
                && std::abs(s->rect.x + s->rect.width - edge) < 1e-3
                && std::abs(s->rect.width - 1) < 1e-3
                && std::abs(s->rect.height - row.h) < 1e-3;
          });
          TEST_CHECK(seam);
          seams += seam ? 1 : 0;
        }
        if (piece.onPath) {
          pathOutlineFollowsTheBar(out, *shape, row.fade, i == 0, i + 1 == count);
        }
      }
      if (row.named) {
        const auto name = std::ranges::find_if(out.labels, [&](const ShapeLabel& l) {
          return l.node == row.folder && l.align == LabelAlign::End;
        });
        TEST_CHECK(name != out.labels.end());
        TEST_CHECK(sameRgb(name->color, kPalette.onSurfaceVariant) && name->color.a < 255 && name->fontSize == 11);
      }
    }
    TEST_CHECK(seams > 4);
    // Far bars fade toward the pane's background by color, so their overlapping parts stay opaque.
    std::size_t faded = 0;
    for (const auto& shape : out.shapes) {
      TEST_CHECK(shape.opacity == 1 && (shape.fill.a == 255 || shape.fill.a == 0));
    }
    for (const auto& row : scene.rows) {
      if (row.fade >= 1) {
        continue;
      }
      for (const auto& piece : row.pieces) {
        const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
        const auto full = mix(nodeColor(fx.tree, piece.node, kSettings, kPalette).accent, kPalette.surface, 0.3);
        const auto low = kPalette.surfaceContainerLow;
        const auto between = [](int value, int a, int b) { return value >= std::min(a, b) && value <= std::max(a, b); };
        TEST_CHECK(!sameRgb(shape->fill, full));
        TEST_CHECK(between(shape->fill.r, full.r, low.r) && between(shape->fill.g, full.g, low.g));
        TEST_CHECK(between(shape->fill.b, full.b, low.b));
        ++faded;
      }
    }
    TEST_CHECK(faded > 0);
  }

  // The linked outline and the hatch of every piece of the nearest bar stay whole: nothing solid draws over them
  // but the 1 px line that parts a piece from the next.
  void testLadderMarksStayVisible() {
    auto fx = luciole::test::sceneFixture();
    const auto scene = ladderScene(fx.tree, kSettings, 900, 200);
    const auto& nearest = *std::ranges::max_element(scene.rows, {}, [](const LadderRow& row) { return row.y; });
    TEST_CHECK(nearest.fade == 1 && nearest.pieces.size() > 2);
    for (const auto& piece : nearest.pieces) {
      const auto out = ladderShapes(fx.tree, kSettings, scene, {.hovered = {}, .linked = piece.node}, kPalette);
      const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
      TEST_CHECK(shape->strokeWidth == 2 && sameRgb(shape->stroke, kPalette.onSurface));
      for (const auto& side : strokeBand(*shape)) {
        TEST_CHECK(!coveredAfter(out, shape, side, 0.01));
      }
    }
    for (const auto& piece : nearest.pieces) {
      fx.tree.nodes[piece.node].partial = true;
    }
    const auto out = ladderShapes(fx.tree, kSettings, scene, {}, kPalette);
    for (const auto& piece : nearest.pieces) {
      const auto* shape = hitTarget(out, {piece.node, SceneRole::Piece});
      TEST_CHECK(shape->pattern == ShapePattern::Hatched && !coveredAfter(out, shape, shape->rect, 1.5));
    }
  }

  // Many small siblings leave a remainder, hatched in the container colors.
  void testLadderRemainder() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "r", .folder = true, .bytes = 4e5, .items = 300},
        {.parent = 0, .name = "here", .folder = true, .bytes = 1e5, .items = 1},
    };
    std::vector<std::string> names(299);
    for (std::size_t i = 0; i < names.size(); ++i) {
      names[i] = std::format("file{:03}.txt", i);
      rows.push_back({.parent = 0, .name = names[i], .bytes = 1000.0 + static_cast<double>(i % 7) * 400});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    const auto scene = ladderScene(tree, kSettings, 544, 100);
    const auto out = ladderShapes(tree, kSettings, scene, {}, kPalette);
    const auto withRest = std::ranges::find_if(scene.rows, [](const LadderRow& row) { return row.rest.has_value(); });
    TEST_CHECK(withRest != scene.rows.end());
    const auto& row = *withRest;
    TEST_CHECK(row.folder == 0 && row.rest->count > 0 && row.restLabeled);
    const auto* rest = hitTarget(out, {0, SceneRole::More});
    TEST_CHECK(rest != nullptr && sameRect(rest->rect, row.rest->x, row.y, row.rest->w, row.h));
    TEST_CHECK(rest->pattern == ShapePattern::Hatched && rest->strokeWidth == 0);
    const auto text = std::format("+{}", row.rest->count);
    TEST_CHECK(std::ranges::any_of(out.labels, [&](const ShapeLabel& label) {
      return label.node == row.folder && label.text == text && label.align == LabelAlign::Center;
    }));
    TEST_CHECK(sameRgb(rest->fill, kPalette.surfaceContainerHigh));
    TEST_CHECK(sameRgb(rest->stroke, kPalette.surfaceContainerHighest));
    TEST_CHECK(rest->rect.radius > 0); // the bar's last piece
    TEST_CHECK(!coveredAfter(out, rest, rest->rect, 1.5));
  }

  void testFlowLook() {
    const auto tree = crowdedTree();
    const auto layout = flowLayout(tree, kSettings, 900, 300);
    const auto out = flowShapes(tree, kSettings, layout, {}, kPalette, LabelFont{});
    const auto& records = layout.topology.records;
    std::size_t more = 0;
    for (const auto& record : records) {
      if (record.kind == FlowRecordKind::Folder || record.kind == FlowRecordKind::Current) {
        const auto* bar = findSceneShape(out, {record.node, SceneRole::Piece});
        TEST_CHECK(bar->kind == ShapeKind::RoundedRect && bar->rect.radius == 3);
        const auto* stripe = firstDrawn(out, {record.node, SceneRole::Detail});
        const auto color = nodeColor(tree, record.node, kSettings, kPalette);
        TEST_CHECK(stripe != nullptr && std::abs(stripe->rect.x - record.x) < 1e-3 && stripe->rect.width == 2.5F);
        TEST_CHECK(sameRgb(stripe->fill, color.accent));
      } else if (record.kind == FlowRecordKind::Files) {
        const SceneKey key{record.node, SceneRole::Files, record.column};
        const auto* bar = findSceneShape(out, key);
        TEST_CHECK(bar->pattern == ShapePattern::Hatched && sameRgb(bar->fill, kPalette.surfaceContainerHigh));
      } else {
        const auto* bar = findSceneShape(out, {record.node, SceneRole::More});
        TEST_CHECK(bar->pattern == ShapePattern::DashedOutline && sameRgb(bar->stroke, kPalette.onSurfaceVariant));
        ++more;
      }
    }
    TEST_CHECK(more > 0);
    for (const auto& ribbon : layout.ribbons) {
      const auto& record = records[ribbon.record];
      const bool muted = record.kind == FlowRecordKind::More;
      const auto* shape = findSceneShape(out, {record.node, muted ? SceneRole::MoreRibbon : SceneRole::Ribbon});
      TEST_NEAR(shape->opacity, muted ? 0.15F : 0.4F, 1e-6);
      if (muted) {
        TEST_CHECK(sameRgb(shape->fill, kPalette.onSurfaceVariant));
      } else {
        const auto color = nodeColor(tree, record.node, kSettings, kPalette);
        TEST_CHECK(sameRgb(shape->fill, mix(color.fill, color.accent, 0.2)));
      }
    }
    for (const auto& shape : out.shapes) {
      if (sceneKeyOf(shape).role == SceneRole::Link) {
        TEST_CHECK(shape.line.width == 2 && sameRgb(shape.fill, kPalette.outlineVariant));
      }
    }
    // Links draw first, under the ribbons.
    const auto link =
        std::ranges::find_if(out.shapes, [](const Shape& s) { return sceneKeyOf(s).role == SceneRole::Link; });
    const auto ribbon = std::ranges::find_if(out.shapes, [](const Shape& s) { return s.kind == ShapeKind::Ribbon; });
    TEST_CHECK(link < ribbon);
  }

  void testFlowFocusAndCursor() {
    auto fx = luciole::test::sceneFixture();
    const auto layout = flowLayout(fx.tree, kSettings, 900, 300);
    const auto& records = layout.topology.records;
    const auto target = std::ranges::find_if(records, [](const FlowRecord& r) {
      return r.kind == FlowRecordKind::Folder && r.column == 1;
    });
    const auto index = static_cast<std::size_t>(target - records.begin());
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = target->node, .role = shapeRole(SceneRole::Piece)};
    const auto out = flowShapes(fx.tree, kSettings, layout, marks, kPalette, LabelFont{});
    const auto lit = flowFocus(layout.topology, index);
    for (std::size_t i = 0; i < records.size(); ++i) {
      if (records[i].kind != FlowRecordKind::Folder) {
        continue;
      }
      const auto* bar = findSceneShape(out, {records[i].node, SceneRole::Piece});
      TEST_NEAR(bar->opacity, lit[i] ? 1.0F : 0.32F, 1e-6);
      const auto* ring = firstDrawn(out, {records[i].node, SceneRole::Mark});
      TEST_CHECK((ring != nullptr) == lit[i]);
      if (ring != nullptr) {
        TEST_CHECK(sameRgb(
            ring->stroke,
            i == index ? kPalette.onSurface : nodeColor(fx.tree, records[i].node, kSettings, kPalette).accent
        ));
      }
    }
    const auto cursor = flowShapes(
        fx.tree, kSettings, layout, {.hovered = {}, .linked = kNoNode, .cursor = target->node}, kPalette, LabelFont{}
    );
    const auto* ring = firstDrawn(cursor, {target->node, SceneRole::Mark});
    TEST_CHECK(ring != nullptr && sameRgb(ring->stroke, kPalette.primary) && ring->strokeWidth == 1.75F);
  }

  // A folder of files alone says so beside its bar.
  void testFlowNote() {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "top", .folder = true, .bytes = 2e5, .items = 1},
        {.parent = 0, .name = "here", .folder = true, .bytes = 1e5, .items = 2},
        {.parent = 1, .name = "a.txt", .bytes = 5e4},
        {.parent = 1, .name = "b.txt", .bytes = 5e4},
    };
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 1;
    tree.ancestors = parentChain(tree, tree.root);
    const auto layout = flowLayout(tree, kSettings, 900, 300);
    TEST_CHECK(layout.topology.empty == FlowEmpty::NoFolders);
    const auto out = flowShapes(tree, kSettings, layout, {}, kPalette, LabelFont{});
    const auto line = [&](std::string_view text) {
      return std::ranges::find_if(out.labels, [&](const ShapeLabel& l) { return l.text == text; });
    };
    const auto title = line("No subfolders here");
    const auto hint = line("Its files are in the list below");
    TEST_CHECK(title != out.labels.end() && hint != out.labels.end());
    TEST_CHECK(title->align == LabelAlign::Center && hint->y > title->y && hint->color.a < 255);
    TEST_CHECK(sameRgb(title->color, kPalette.onSurfaceVariant));
    TEST_CHECK(title->x > layout.topology.records[0].x + layout.topology.records[0].width);
    // A faint folder icon stands centered over the title.
    TEST_CHECK(title->icon.category == FileCategory::Folder && title->icon.size == 24);
    TEST_NEAR(title->icon.left + 12, title->width / 2, 1e-4);
    TEST_CHECK(title->icon.top + 24 < 0 && title->icon.color.a < 255);
    TEST_CHECK(hint->icon.size == 0);
  }

  // The diagram carries the note of every folder it has no folder record for, once, beside the folder's bar: the
  // flow part shows no other note.
  void testFlowNoteForEveryState() {
    const auto notes = [](std::vector<WorkbenchRow> kids, bool unreadable, bool truncated) {
      std::vector<WorkbenchRow> rows{
          {.parent = -1, .name = "top", .folder = true, .bytes = 2e5, .items = 1},
          {.parent = 0,
           .name = "here",
           .folder = true,
           .bytes = 1e5,
           .items = truncated ? 12 : static_cast<double>(kids.size()),
           .unreadable = unreadable,
           .truncated = truncated},
      };
      rows.insert(rows.end(), kids.begin(), kids.end());
      auto tree = luciole::test::workbenchTree(rows, 0);
      tree.root = 1;
      tree.ancestors = parentChain(tree, tree.root);
      const auto layout = flowLayout(tree, kSettings, 900, 300);
      const auto out = flowShapes(tree, kSettings, layout, {}, kPalette, LabelFont{});
      std::vector<std::string> centered;
      for (const auto& label : out.labels) {
        if (label.node == tree.root && label.align == LabelAlign::Center && label.fontSize == 13) {
          centered.push_back(label.text);
        }
      }
      return centered;
    };
    const WorkbenchRow file{.parent = 1, .name = "a.txt", .bytes = 5e4};
    const WorkbenchRow hidden{.parent = 1, .name = ".a", .bytes = 5e4};
    TEST_CHECK((notes({}, false, false) == std::vector<std::string>{"This folder is empty"}));
    TEST_CHECK((notes({}, true, false) == std::vector<std::string>{"This folder could not be read"}));
    TEST_CHECK((notes({file}, true, false) == std::vector<std::string>{"This folder could not be read"}));
    TEST_CHECK((notes({}, false, true) == std::vector<std::string>{"Not expanded in this snapshot"}));
    TEST_CHECK((notes({hidden}, false, false) == std::vector<std::string>{"No subfolders here"}));
  }
} // namespace

int main() {
  testFlameBarPinsAndMore();
  testFlameFocus();
  testLadderSegments();
  testLadderMarksStayVisible();
  testLadderRemainder();
  testFlowLook();
  testFlowFocusAndCursor();
  testFlowNote();
  testFlowNoteForEveryState();
  return 0;
}
