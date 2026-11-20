#pragma once

#include "test_check.h"
#include "workbench/flame_row.h"
#include "workbench/flame_strata.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::test::flame {
  using namespace workbench;

  inline constexpr double kEps = 1e-6;
  inline constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  inline constexpr double kInfinity = std::numeric_limits<double>::infinity();

  struct Row {
    int parent = -1;
    std::string name;
    bool folder = true;
    double bytes = 0;
  };

  // Rows list parents first; children keep their row order. Linear in the row count.
  inline Tree buildTree(const std::vector<Row>& rows) {
    Tree tree;
    tree.nodes.resize(rows.size());
    std::vector<std::uint32_t> counts(rows.size(), 0);
    for (const auto& row : rows) {
      if (row.parent >= 0) {
        ++counts[static_cast<std::size_t>(row.parent)];
      }
    }
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      auto& node = tree.nodes[i];
      node.name = rows[i].name;
      node.parent = rows[i].parent < 0 ? kNoNode : static_cast<NodeId>(rows[i].parent);
      node.folder = rows[i].folder;
      node.bytes = rows[i].bytes;
      node.items = rows[i].folder ? 1 : 0;
      node.modified = 1000;
      node.hidden = rows[i].parent >= 0 && node.name.starts_with('.');
      node.firstChild = offset;
      node.childCount = counts[i];
      offset += counts[i];
    }
    tree.children.resize(offset);
    std::vector<std::uint32_t> filled(rows.size(), 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].parent >= 0) {
        const auto parent = static_cast<std::size_t>(rows[i].parent);
        tree.children[tree.nodes[parent].firstChild + filled[parent]++] = static_cast<NodeId>(i);
      }
    }
    tree.root = 0;
    tree.now = 2000;
    return tree;
  }

  inline void enter(Tree& tree, NodeId folder) {
    tree.root = folder;
    tree.ancestors = parentChain(tree, folder);
  }

  inline NodeId childAt(const Tree& tree, NodeId parent, std::size_t index) { return children(tree, parent)[index]; }

  // /w: 40 folders d<i> of falling size, each with 10 subfolders (5 files each) and 20 files; 2000 tiny folders t<i>
  // with one tiny subfolder each; 3000 files that are never drawn.
  inline Tree wideTree() {
    std::vector<Row> rows{{.parent = -1, .name = "w"}};
    const auto add = [&rows](int parent, std::string name, bool folder, double bytes) {
      rows.push_back({.parent = parent, .name = std::move(name), .folder = folder, .bytes = bytes});
      return static_cast<int>(rows.size() - 1);
    };
    for (int i = 0; i < 40; ++i) {
      const int d = add(0, std::format("d{}", i), true, 1e6 / (i + 1));
      for (int j = 0; j < 10; ++j) {
        const int e = add(d, std::format("e{}", j), true, 1e5 / (j + 1));
        for (int k = 0; k < 5; ++k) {
          add(e, std::format("f{}", k), false, 1e4 / (k + 1));
        }
      }
      for (int j = 0; j < 20; ++j) {
        add(d, std::format("g{}", j), false, 1e3);
      }
    }
    for (int i = 0; i < 2000; ++i) {
      const int t = add(0, std::format("t{}", i), true, 10);
      add(t, "u", true, 1);
    }
    for (int i = 0; i < 3000; ++i) {
      add(0, std::format("file{}", i), false, 5e5);
    }
    return buildTree(rows);
  }

  // /t: a has b has c has d and e; a2 beside a; b2 beside b.
  inline Tree chainTree() {
    return buildTree({
        {.parent = -1, .name = "t"},
        {.parent = 0, .name = "a", .bytes = 1000},
        {.parent = 1, .name = "b", .bytes = 800},
        {.parent = 2, .name = "c", .bytes = 500},
        {.parent = 3, .name = "d", .bytes = 300},
        {.parent = 3, .name = "e", .bytes = 100},
        {.parent = 0, .name = "a2", .bytes = 200},
        {.parent = 1, .name = "b2", .bytes = 100},
    });
  }

  inline FlameFonts sevenPixelFonts() {
    LabelFont font;
    font.ascii.fill(7);
    font.other = 7;
    return {.firstAhead = font, .ahead = font, .ancestor = font, .value = font};
  }

  inline std::string itemName(const Tree& tree, const FlameItem& item) {
    if (item.bar) {
      return "bar";
    }
    return item.more ? std::format("+{}", item.list.size()) : tree.nodes[item.node].name;
  }

  struct Expected {
    std::string_view name;
    double x = 0;
    double w = 0;
  };

  inline void checkItems(const Tree& tree, const std::vector<FlameItem>& items, const std::vector<Expected>& expected) {
    TEST_CHECK(items.size() >= expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      TEST_CHECK(itemName(tree, items[i]) == expected[i].name);
      TEST_NEAR(items[i].x, expected[i].x, kEps);
      TEST_NEAR(items[i].w, expected[i].w, kEps);
    }
  }
} // namespace luciole::test::flame
