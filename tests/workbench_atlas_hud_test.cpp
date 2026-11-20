#include "test_check.h"
#include "workbench/atlas_hud.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  void checkRect(const Rect& rect, double x, double y, double width, double height) {
    TEST_NEAR(rect.x, x, 1e-9);
    TEST_NEAR(rect.y, y, 1e-9);
    TEST_NEAR(rect.width, width, 1e-9);
    TEST_NEAR(rect.height, height, 1e-9);
  }

  void checkEntry(
      const HudEntry& entry, HudEntryKind kind, std::size_t levels, double maxWidth, double font, double opacity,
      std::size_t chars
  ) {
    TEST_CHECK(entry.kind == kind);
    TEST_CHECK(entry.levels == levels);
    TEST_NEAR(entry.maxWidth, maxWidth, 0.0);
    TEST_NEAR(entry.fontSize, font, 0.0);
    TEST_NEAR(entry.opacity, opacity, 1e-12);
    TEST_CHECK(entry.maxChars == chars);
  }

  void testWideHud() {
    const auto layout = hudLayout({1000, 800}, 3);
    TEST_CHECK(!layout.compact);
    checkRect(layout.map, 0, 0, 1000, 800);
    checkRect(layout.chipArea, 12, 12, 976, 776);
    checkRect(layout.buttons[0], 958, 690, 30, 30);
    checkRect(layout.buttons[1], 958, 724, 30, 30);
    checkRect(layout.buttons[2], 958, 758, 30, 30);
    TEST_CHECK(layout.entries.size() == 4);
    checkEntry(layout.entries[0], HudEntryKind::Ancestor, 3, 130, 12, 0.76, 18);
    checkEntry(layout.entries[1], HudEntryKind::Ancestor, 2, 170, 12, 0.88, 24);
    checkEntry(layout.entries[2], HudEntryKind::Ancestor, 1, 220, 13, 1, 31);
    checkEntry(layout.entries[3], HudEntryKind::Current, 0, 976, 13, 1, 30);
    const auto rects = placeHud(layout, std::vector<Extent>{{100, 24}, {150, 24}, {300, 24}, {80, 23}});
    TEST_CHECK(rects.size() == 4);
    checkRect(rects[0], 12, 12, 100, 24);
    checkRect(rects[1], 12, 39, 150, 24);
    checkRect(rects[2], 12, 66, 220, 24);
    checkRect(rects[3], 12, 93, 80, 23);

    const auto deep = hudLayout({1000, 800}, 7);
    TEST_CHECK(deep.entries.size() == 7);
    checkEntry(deep.entries[0], HudEntryKind::MoreLevels, 2, 976, 11, 1, 0);
    checkEntry(deep.entries[1], HudEntryKind::Ancestor, 5, 80, 11, 0.55, 11);
    checkEntry(deep.entries[2], HudEntryKind::Ancestor, 4, 100, 11, 0.64, 14);
    checkEntry(deep.entries[5], HudEntryKind::Ancestor, 1, 220, 13, 1, 31);
    TEST_CHECK(deep.entries[6].kind == HudEntryKind::Current);
    const auto top = hudLayout({1000, 800}, 0);
    TEST_CHECK(top.entries.size() == 1 && top.entries[0].kind == HudEntryKind::Current);
    TEST_CHECK(!hudLayout({460, 460}, 1).compact);
  }

  void testCompactHud() {
    const auto layout = hudLayout({340, 380}, 6);
    TEST_CHECK(layout.compact);
    checkRect(layout.map, 0, 26, 340, 354);
    checkRect(layout.chipArea, 4, 3, 252, 20);
    checkRect(layout.buttons[0], 260, 3, 24, 20);
    checkRect(layout.buttons[1], 286, 3, 24, 20);
    checkRect(layout.buttons[2], 312, 3, 24, 20);
    TEST_CHECK(layout.entries.size() == 4);
    checkEntry(layout.entries[0], HudEntryKind::Ancestor, 4, 62, 11, 0.64, 8);
    checkEntry(layout.entries[3], HudEntryKind::Ancestor, 1, 62, 11, 1, 8);
    const auto rects = placeHud(layout, std::vector<Extent>{{62, 18}, {62, 18}, {62, 18}, {62, 18}});
    checkRect(rects[0], 4, 3, 60.75, 20);
    checkRect(rects[1], 67.75, 3, 60.75, 20);
    checkRect(rects[3], 195.25, 3, 60.75, 20);
    TEST_CHECK(rects[3].x + rects[3].width <= layout.buttons[0].x);

    const auto narrow = hudLayout({260, 240}, 2);
    TEST_CHECK(narrow.entries.size() == 2);
    checkEntry(narrow.entries[0], HudEntryKind::Ancestor, 2, 56, 11, 0.88, 8);
    const auto loose = placeHud(narrow, std::vector<Extent>{{56, 18}, {20, 18}});
    checkRect(loose[0], 4, 3, 56, 20);
    checkRect(loose[1], 63, 3, 28, 20);

    const auto wideShort = hudLayout({600, 240}, 9);
    TEST_CHECK(wideShort.compact && wideShort.entries.size() == 5);
    checkEntry(wideShort.entries[0], HudEntryKind::Ancestor, 5, 102, 11, 0.55, 14);
    TEST_CHECK(hudLayout({459.5, 900}, 1).compact);

    // Chips shrink in proportion to their width; one that reaches 28 px stops and the others give the rest.
    const auto small = hudLayout({200, 300}, 3);
    checkEntry(small.entries[0], HudEntryKind::Ancestor, 3, 44, 11, 0.76, 6);
    const auto shrunk = placeHud(small, std::vector<Extent>{{44, 18}, {44, 18}, {29, 18}});
    checkRect(shrunk[0], 4, 3, 39, 20);
    checkRect(shrunk[1], 46, 3, 39, 20);
    checkRect(shrunk[2], 88, 3, 28, 20);
    const auto floor = placeHud(hudLayout({150, 300}, 3), std::vector<Extent>{{44, 18}, {44, 18}, {30, 18}});
    TEST_CHECK(floor.size() == 3 && floor[0].width == 28 && floor[1].width == 28 && floor[2].width == 28);
  }

  void testDegenerateHud() {
    const auto tiny = hudLayout({50, 50}, 4);
    TEST_CHECK(tiny.compact);
    checkRect(tiny.map, 0, 26, 50, 24);
    checkRect(tiny.chipArea, 4, 3, 0, 20);
    checkEntry(tiny.entries[0], HudEntryKind::Ancestor, 3, 44, 11, 0.76, 6);
    const auto broken = hudLayout({kNan, -10}, 2);
    TEST_CHECK(broken.compact);
    checkRect(broken.map, 0, 26, 0, 0);
    TEST_CHECK(hudLayout({1000, 800}, std::numeric_limits<std::size_t>::max()).entries.size() == 7);
    const auto layout = hudLayout({1000, 800}, 3);
    const auto partial = placeHud(layout, std::vector<Extent>{{100, 24}});
    TEST_CHECK(partial.size() == 4);
    checkRect(partial[1], 12, 39, 0, 0);
    const auto bad = placeHud(layout, std::vector<Extent>{{kNan, kInfinity}, {-5, -5}});
    checkRect(bad[0], 12, 12, 0, 0);
    checkRect(bad[1], 12, 15, 0, 0);
    TEST_CHECK(placeHud(hudLayout({340, 380}, 0), {}).empty());
  }
  // Every ASCII character 6 px wide at 12 px, any other code point 12 px.
  LabelFont evenFont() {
    LabelFont font;
    font.ascii.fill(6);
    font.other = 12;
    return font;
  }

  // top ("") > usr > share > a folder with a 40 character name.
  Tree pathTree() {
    Tree tree;
    for (const auto* name : {"", "usr", "share", "a-folder-named-with-forty-characters-xyz"}) {
      Node node;
      node.name = name;
      node.folder = true;
      node.parent = tree.nodes.empty() ? kNoNode : static_cast<NodeId>(tree.nodes.size() - 1);
      node.childCount = 1;
      node.firstChild = static_cast<std::uint32_t>(tree.nodes.size());
      tree.nodes.push_back(node);
      if (tree.nodes.size() > 1) {
        tree.children.push_back(static_cast<NodeId>(tree.nodes.size() - 1));
      }
    }
    tree.nodes.back().childCount = 0;
    tree.root = 3;
    tree.ancestors = {2, 1, 0};
    return tree;
  }

  // A chip's text: the folder's name cut to the entry's length, "/" for the top; "+N levels" for the hidden ones.
  void testHudText() {
    const auto tree = pathTree();
    const auto wide = hudLayout({1000, 800}, 3);
    TEST_CHECK(hudText(tree, wide.entries[0]) == "/");
    TEST_CHECK(hudText(tree, wide.entries[1]) == "usr");
    TEST_CHECK(hudText(tree, wide.entries[2]) == "share");
    TEST_CHECK(hudText(tree, wide.entries[3]) == "a-folder-named-\u2026characters-xyz");
    const auto deep = hudLayout({1000, 800}, 7);
    TEST_CHECK(hudText(tree, deep.entries[0]) == "+2 levels");
    // An ancestor the tree does not have reads as no text.
    TEST_CHECK(hudText(tree, deep.entries[1]).empty());
  }

  // Natural chip sizes: the text but at least 18 px, padding each side and a pixel of border each side; 1.35 lines
  // and 6 px tall.
  void testHudExtents() {
    const auto tree = pathTree();
    const auto font = evenFont();
    const auto wide = hudLayout({1000, 800}, 3);
    const auto sizes = hudExtents(tree, wide, font);
    TEST_CHECK(sizes.size() == 4);
    TEST_NEAR(sizes[0].width, 18.0 + 18 + 2, 1e-9);
    TEST_NEAR(sizes[0].height, 12 * 1.35 + 6, 1e-9);
    TEST_NEAR(sizes[1].width, 18.0 + 18 + 2, 1e-9);
    TEST_NEAR(hudExtents(tree, hudLayout({1000, 800}, 2), font)[1].width, 32.5 + 18 + 2, 1e-9);
    TEST_NEAR(sizes[2].width, 32.5 + 18 + 2, 1e-9);
    TEST_NEAR(sizes[2].height, 13 * 1.35 + 6, 1e-9);
    TEST_NEAR(sizes[3].width, 29 * 6.5 + 13 + 18 + 2, 1e-9);
    const auto deep = hudLayout({1000, 800}, 7);
    TEST_NEAR(hudExtents(tree, deep, font)[0].width, 9 * 5.5 + 4 + 2, 1e-9);
    const auto compact = hudLayout({340, 380}, 3);
    const auto bar = hudExtents(tree, compact, font);
    TEST_CHECK(bar.size() == 3);
    TEST_NEAR(bar[0].width, 18.0 + 12 + 2, 1e-9);
    TEST_NEAR(bar[0].height, 20.0, 1e-9);
  }

  void testHudPath() {
    const auto tree = pathTree();
    TEST_CHECK(hudPath(tree, 2) == "/usr/share");
    TEST_CHECK(hudPath(tree, 0) == "/");
    TEST_CHECK(hudPath(tree, kNoNode).empty());
    auto named = tree;
    named.nodes[0].name = "/";
    TEST_CHECK(hudPath(named, 1) == "/usr");
  }
} // namespace

int main() {
  testWideHud();
  testCompactHud();
  testDegenerateHud();
  testHudText();
  testHudExtents();
  testHudPath();
  std::println("workbench atlas hud checks passed");
  return 0;
}
