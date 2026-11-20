#include "test_check.h"
#include "workbench/atlas_camera.h"
#include "workbench/atlas_hud.h"
#include "workbench/format.h"
#include "workbench/scene_shapes.h"
#include "workbench_scene_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The drawn look of the Atlas and its heads-up chips and buttons.
namespace {
  using namespace luciole::workbench;
  using luciole::test::drawn;
  using luciole::test::findSceneShape;
  using luciole::test::firstDrawn;
  using luciole::test::hitTarget;
  using luciole::test::sameRect;
  using luciole::test::sameRgb;

  const LayoutSettings kSettings{};
  const ColorPalette kPalette{};

  // Every ASCII character 6 px wide at 12 px.
  LabelFont evenFont() {
    LabelFont font;
    font.ascii.fill(6);
    font.other = 12;
    return font;
  }
  const LabelFont kFont = evenFont();

  const ShapeLabel* labelWithText(const SceneShapes& out, std::string_view text) {
    const auto found = std::ranges::find(out.labels, text, &ShapeLabel::text);
    return found == out.labels.end() ? nullptr : &*found;
  }

  std::vector<const Shape*> detailsOf(const SceneShapes& out, NodeId node) {
    std::vector<const Shape*> found;
    for (const auto& shape : out.shapes) {
      if (shape.id.node == node && sceneRole(shape.id.role) == SceneRole::Detail) {
        found.push_back(&shape);
      }
    }
    return found;
  }

  // The current folder at rest gets a chip in its cap: an accent dot, the name and the measure, sized to the text.
  void testTitleChip() {
    auto fx = luciole::test::sceneFixture();
    AtlasScene scene;
    scene.view = {.width = 900, .height = 600};
    scene.camera = {.cx = 0, .cy = 0, .scale = 286};
    scene.items.push_back({.node = fx.work, .role = BubbleRole::Self, .x = 450, .y = 300, .r = 286, .clickable = true});
    const auto out = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    // "work" at 14 px is 28 px and "2.9 MiB" at 11 px 44 px (its no-break space counts as one wide code point):
    // 28 + 22 + (44 + 8) + 12 = 114 px wide.
    const auto details = detailsOf(out, fx.work);
    TEST_CHECK(details.size() == 2);
    const auto* chip = details[0];
    TEST_CHECK(chip->kind == ShapeKind::RoundedRect && !chip->hittable);
    TEST_CHECK(sameRect(chip->rect, 393, 27.31, 114, 22));
    TEST_CHECK(sameRgb(chip->fill, kPalette.surface) && chip->fill.a < 255);
    const auto* dot = details[1];
    TEST_CHECK(dot->kind == ShapeKind::Circle);
    TEST_NEAR(dot->circle.center.x, 404.0F, 1e-3F);
    TEST_NEAR(dot->circle.center.y, 38.31F, 1e-3F);
    const auto* name = labelWithText(out, "work");
    TEST_CHECK(name != nullptr && name->align == LabelAlign::Start);
    TEST_NEAR(name->x, 413.0F, 1e-3F);
    TEST_NEAR(name->fontSize, 14.0F, 1e-6F);
    TEST_CHECK(name->width >= 28);
    TEST_CHECK(name->weight == LabelWeight::Semibold && name->fit == LabelFit::Shorten);
    const auto* metric = labelWithText(out, "2.9\u00A0MiB");
    TEST_CHECK(metric != nullptr);
    TEST_NEAR(metric->x, 449.0F, 1e-3F);
    TEST_NEAR(metric->fontSize, 11.0F, 1e-6F);
    TEST_CHECK(metric->fit == LabelFit::Whole && metric->weight == LabelWeight::Regular);
    // A cap too narrow for the measure keeps the name alone.
    scene.items[0].r = 120;
    const auto narrow = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    TEST_CHECK(labelWithText(narrow, "work") != nullptr);
    TEST_CHECK(labelWithText(narrow, "2.9\u00A0MiB") == nullptr);
  }

  // A folder whose children are drawn names itself on a pill: icon, semibold name and, at full strength, the whole
  // measure, the pill sized to them; a measure still fading in leaves the name alone.
  void testNamePill() {
    auto fx = luciole::test::sceneFixture();
    const NodeId p0 = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    AtlasScene scene;
    scene.view = {.width = 900, .height = 600};
    scene.camera = {.cx = 0, .cy = 0, .scale = 286};
    scene.items.push_back(
        {.node = p0,
         .role = BubbleRole::Bubble,
         .x = 200,
         .y = 200,
         .r = 80,
         .depth = 1,
         .clickable = true,
         .kids = true}
    );
    const auto out = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    // Icon 18 + name 17 + measure 15 = 50 px tall. The 13 px name is 13 px wide and the icon takes 28, so the 44 px
    // of "1.5 MiB" at 11 px set the width.
    auto details = detailsOf(out, p0);
    TEST_CHECK(details.size() == 1);
    TEST_CHECK(details[0]->kind == ShapeKind::RoundedRect && sameRect(details[0]->rect, 170, 171, 60, 58));
    TEST_NEAR(details[0]->rect.radius, 9.0F, 1e-6F);
    const auto* name = labelWithText(out, "p0");
    TEST_CHECK(name != nullptr && name->align == LabelAlign::Center);
    TEST_CHECK(name->weight == LabelWeight::Semibold && name->fit == LabelFit::Shorten);
    TEST_NEAR(name->x + name->width / 2, 200.0F, 1e-3F);
    TEST_NEAR(name->y + name->height / 2, 201.5F, 1e-3F);
    TEST_CHECK(name->width >= 18); // narrower labels are not drawn
    TEST_CHECK(sameRgb(name->color, kPalette.onSurface));
    TEST_CHECK(name->icon.size == 16);
    TEST_NEAR(name->y + name->icon.top + 8, 184.0F, 1e-3F);
    TEST_NEAR(name->x + name->icon.left + 8, 200.0F, 1e-3F);
    const auto* metric = labelWithText(out, "1.5\u00A0MiB");
    TEST_CHECK(metric != nullptr && metric->fit == LabelFit::Whole && metric->weight == LabelWeight::Regular);
    TEST_NEAR(metric->y + metric->height / 2, 216.5F, 1e-3F);
    // At 48 px the measure is still fading in, too faint for a pill: the name shows alone, at 12 px. Icon 18 + name
    // 16 = 34 px tall, the icon's 28 px wide.
    scene.items[0].r = 48;
    const auto fading = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    details = detailsOf(fading, p0);
    TEST_CHECK(details.size() == 1 && sameRect(details[0]->rect, 178, 179, 44, 42));
    name = labelWithText(fading, "p0");
    TEST_CHECK(name != nullptr);
    TEST_NEAR(name->y + name->height / 2, 209.0F, 1e-3F);
    TEST_CHECK(labelWithText(fading, "1.5\u00A0MiB") == nullptr);
    scene.items[0].r = 80;
    // Without children drawn there is no pill.
    scene.items[0].kids = false;
    TEST_CHECK(detailsOf(atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont), p0).empty());
  }

  // "+N" over "more", centered on the bubble.
  void testMoreBubble() {
    auto fx = luciole::test::sceneFixture();
    AtlasScene scene;
    scene.view = {.width = 900, .height = 600};
    scene.camera = {.cx = 0, .cy = 0, .scale = 286};
    scene.items.push_back(
        {.folder = fx.work, .role = BubbleRole::More, .x = 100, .y = 100, .r = 40, .restCount = 1234, .clickable = true}
    );
    const auto out = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    const auto* count = labelWithText(out, "+1,234");
    TEST_CHECK(count != nullptr);
    TEST_NEAR(count->y + count->height / 2, 94.0F, 1e-3F);
    TEST_NEAR(count->x + count->width / 2, 100.0F, 1e-3F);
    TEST_NEAR(count->width, 44.12F, 1e-3F); // 39 px of text, 8% more as drawn text runs wider, and 2 px
    TEST_CHECK(count->fit == LabelFit::Whole && count->weight == LabelWeight::Semibold);
    const auto* more = labelWithText(out, "more");
    TEST_CHECK(more != nullptr);
    TEST_NEAR(more->y + more->height / 2, 109.0F, 1e-3F);
    TEST_NEAR(more->fontSize, 11.0F, 1e-6F);
    TEST_CHECK(more->fit == LabelFit::Whole);
    // Under 25 px the bubble is too small for either line.
    scene.items[0].r = 24;
    const auto small = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    TEST_CHECK(labelWithText(small, "+1,234") == nullptr);
    TEST_CHECK(labelWithText(small, "more") == nullptr);
  }

  // When more circles are labeled than the scene keeps labels for, the pills of the dropped labels go too.
  void testPillsFollowKeptLabels() {
    std::vector<luciole::test::WorkbenchRow> rows{{.parent = -1, .name = "top", .folder = true, .bytes = 1000}};
    std::vector<std::string> names;
    names.reserve(100);
    for (int i = 0; i < 100; ++i) {
      names.push_back(std::format("folder-{:02}", i)); // wide enough for its pill to hold the measure
    }
    for (const auto& name : names) {
      rows.push_back({.parent = 0, .name = name, .folder = true, .bytes = 10});
    }
    const auto tree = luciole::test::workbenchTree(rows, 0);
    AtlasScene scene;
    scene.view = {.width = 2000, .height = 2000};
    scene.camera = {.cx = 0, .cy = 0, .scale = 900};
    for (NodeId id = 1; id <= 100; ++id) {
      const double r = 60 + static_cast<double>(id) * 0.1;
      const double x = 100 + static_cast<double>((id - 1) % 10) * 180;
      const double y = 100 + static_cast<double>((id - 1) / 10) * 180;
      scene.items.push_back(
          {.node = id, .role = BubbleRole::Bubble, .x = x, .y = y, .r = r, .depth = 1, .clickable = true, .kids = true}
      );
    }
    const auto out = atlasShapes(tree, kSettings, scene, {}, kPalette, kFont);
    TEST_CHECK(out.labels.size() == kMaxSceneLabels);
    std::size_t pills = 0;
    for (const auto& shape : out.shapes) {
      if (sceneRole(shape.id.role) != SceneRole::Detail) {
        continue;
      }
      ++pills;
      // Each item gives a name and a measure, so the 80 largest keep theirs: ids 21 to 100.
      TEST_CHECK(shape.id.node >= 21);
      TEST_CHECK(std::ranges::any_of(out.labels, [&shape](const ShapeLabel& label) {
        return label.node == shape.id.node;
      }));
    }
    TEST_CHECK(pills == 80);
  }

  void testAtlasLook() {
    auto fx = luciole::test::sceneFixture();
    PackingCache packings;
    const AtlasSource source{.tree = fx.tree, .settings = kSettings, .packings = packings};
    const ViewSize view{.width = 900, .height = 600};
    const auto scene = buildScene(source, fx.work, restCamera(view), view);
    const auto out = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    std::size_t folders = 0;
    std::size_t files = 0;
    std::size_t ancestors = 0;
    for (const auto& item : scene.items) {
      if (item.role == BubbleRole::More) {
        TEST_CHECK(firstDrawn(out, {item.folder, SceneRole::More})->pattern == ShapePattern::DashedOutline);
        continue;
      }
      const auto* shape = firstDrawn(out, {item.node, SceneRole::Piece});
      TEST_CHECK(shape != nullptr);
      TEST_NEAR(shape->opacity, item.fade, 1e-6);
      if (item.role == BubbleRole::Ancestor) {
        // Ancestors are quiet translucent fills; the ones still drawn as circles are outlined.
        TEST_CHECK(sameRgb(shape->fill, kPalette.surfaceContainerLow) && shape->fill.a < 255);
        TEST_CHECK(item.huge || (shape->strokeWidth > 0 && sameRgb(shape->stroke, kPalette.outlineVariant)));
        ++ancestors;
      } else if (item.role == BubbleRole::Self) {
        TEST_CHECK(shape->fill.a < 255 && sameRgb(shape->stroke, kPalette.secondary));
      } else if (fx.tree.nodes[item.node].folder) {
        // A folder is a translucent fill with an accent outline, inside its radius.
        const auto color = nodeColor(fx.tree, item.node, kSettings, kPalette);
        TEST_CHECK(shape->fill.a < 200 && shape->fill.a > 0 && sameRgb(shape->fill, color.fill));
        TEST_CHECK(shape->strokeWidth >= 1 && sameRgb(shape->stroke, color.accent));
        TEST_CHECK(shape->circle.radius < item.r && shape->circle.radius >= item.r - 2 - 1e-3);
        ++folders;
      } else {
        TEST_CHECK(shape->fill.a > 240 && shape->strokeWidth == 0);
        ++files;
      }
      if (item.clickable) {
        const auto* hit = hitTarget(out, {item.node, SceneRole::Piece});
        TEST_CHECK(hit != nullptr && hit->fill.a == 0);
      }
    }
    TEST_CHECK(folders > 8 && files > 8 && ancestors > 0);
    // A partial folder is hatched.
    TEST_CHECK(firstDrawn(out, {fx.partial, SceneRole::Piece})->pattern == ShapePattern::Hatched);
    // So is a folder whose totals are a lower bound, and a finished one is not.
    auto& marked = fx.tree.nodes[fx.partial];
    marked.partial = false;
    marked.lowerBound = true;
    const auto bounded = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    TEST_CHECK(firstDrawn(bounded, {fx.partial, SceneRole::Piece})->pattern == ShapePattern::Hatched);
    marked.lowerBound = false;
    const auto finished = atlasShapes(fx.tree, kSettings, scene, {}, kPalette, kFont);
    TEST_CHECK(firstDrawn(finished, {fx.partial, SceneRole::Piece})->pattern != ShapePattern::Hatched);
  }

  void testAtlasMarks() {
    auto fx = luciole::test::sceneFixture();
    PackingCache packings;
    const AtlasSource source{.tree = fx.tree, .settings = kSettings, .packings = packings};
    const ViewSize view{.width = 900, .height = 600};
    const auto scene = buildScene(source, fx.work, restCamera(view), view);
    const NodeId heavy = fx.tree.children[fx.tree.nodes[fx.work].firstChild];
    const auto item = std::ranges::find_if(scene.items, [heavy](const SceneItem& it) { return it.node == heavy; });
    TEST_CHECK(item != scene.items.end());
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = heavy, .role = shapeRole(SceneRole::Piece)};
    const auto hovered = atlasShapes(fx.tree, kSettings, scene, marks, kPalette, kFont);
    const auto* hot = firstDrawn(hovered, {heavy, SceneRole::Mark});
    TEST_CHECK(hot != nullptr && sameRgb(hot->stroke, kPalette.onSurface) && hot->fill.a > 0 && hot->fill.a < 64);
    TEST_NEAR(hot->circle.radius, item->r + 1, 1e-3);
    const auto linked = atlasShapes(fx.tree, kSettings, scene, {.hovered = {}, .linked = heavy}, kPalette, kFont);
    const auto* focus = firstDrawn(linked, {heavy, SceneRole::Mark});
    TEST_CHECK(sameRgb(focus->stroke, kPalette.tertiary) && focus->pattern == ShapePattern::DashedOutline);
    TEST_CHECK(focus->circle.radius > item->r + 3);
    const auto cursor =
        atlasShapes(fx.tree, kSettings, scene, {.hovered = {}, .linked = kNoNode, .cursor = heavy}, kPalette, kFont);
    const auto* ring = firstDrawn(cursor, {heavy, SceneRole::Mark});
    TEST_CHECK(sameRgb(ring->stroke, kPalette.primary) && ring->strokeWidth == 3);
    TEST_CHECK(ring->circle.radius > item->r + 4);
  }

  void testAtlasHud() {
    auto fx = luciole::test::sceneFixture();
    const auto layout = hudLayout({.width = 1000, .height = 800}, fx.tree.ancestors.size());
    const std::vector<Extent> natural(layout.entries.size(), Extent{.width = 90, .height = 20});
    const auto placed = placeHud(layout, natural);
    const auto out = atlasHudShapes(fx.tree, layout, placed, {}, kPalette);
    std::size_t chips = 0;
    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
      const auto& entry = layout.entries[i];
      const auto& rect = placed[i];
      if (entry.kind == HudEntryKind::Ancestor) {
        const NodeId node = fx.tree.ancestors[entry.levels - 1];
        const auto* chip = hitTarget(out, {node, SceneRole::Chip});
        TEST_CHECK(chip != nullptr && sameRect(chip->rect, rect.x, rect.y, rect.width, rect.height));
        TEST_CHECK(sameRgb(chip->fill, kPalette.surface) && chip->fill.a < 255);
        TEST_CHECK(sameRgb(chip->stroke, kPalette.outlineVariant) && chip->strokeWidth == 1);
        TEST_NEAR(chip->opacity, entry.opacity, 1e-6);
        const auto name = middleEllipsis(fx.tree.nodes[node].name, entry.maxChars);
        TEST_CHECK(std::ranges::any_of(out.labels, [&](const ShapeLabel& l) {
          return l.node == node && l.text == name;
        }));
        ++chips;
      } else if (entry.kind == HudEntryKind::Current) {
        const auto* current = findSceneShape(out, {fx.tree.root, SceneRole::Chip});
        TEST_CHECK(current != nullptr && !current->hittable && sameRgb(current->stroke, kPalette.secondary));
      }
    }
    TEST_CHECK(chips == fx.tree.ancestors.size());
    // Three buttons, each a hit target holding its icon: plus, minus, and a target of four ticks and a ring.
    const std::array<std::size_t, 3> strokes{2, 1, 5};
    for (std::size_t k = 0; k < layout.buttons.size(); ++k) {
      const auto& box = layout.buttons[k];
      const auto button = std::ranges::find_if(out.shapes, [k](const Shape& shape) {
        return shape.hittable && shape.id.role == shapeRole(SceneRole::Button, k);
      });
      TEST_CHECK(button != out.shapes.end() && sameRect(button->rect, box.x, box.y, box.width, box.height));
      const auto icon = std::ranges::count_if(out.shapes, [&](const Shape& shape) {
        return shape.id.role == shapeRole(SceneRole::Detail, k)
            && !shape.hittable
            && shapeContains(button[0], shape.kind == ShapeKind::Line ? shape.line.from : shape.circle.center, 0);
      });
      TEST_CHECK(static_cast<std::size_t>(icon) == strokes[k]);
    }
    // Hovering the second button raises it alone.
    ShapeMarks marks;
    marks.hovered = ShapeId{.node = kNoNode, .role = shapeRole(SceneRole::Button, 1)};
    const auto hot = atlasHudShapes(fx.tree, layout, placed, marks, kPalette);
    for (std::size_t k = 0; k < 3; ++k) {
      const auto button = std::ranges::find_if(hot.shapes, [k](const Shape& shape) {
        return shape.hittable && shape.id.role == shapeRole(SceneRole::Button, k);
      });
      TEST_CHECK(sameRgb(button->fill, kPalette.surfaceContainerHigh) == (k == 1));
    }
  }
} // namespace

int main() {
  testAtlasLook();
  testAtlasMarks();
  testAtlasHud();
  testTitleChip();
  testNamePill();
  testMoreBubble();
  testPillsFollowKeptLabels();
  return 0;
}
