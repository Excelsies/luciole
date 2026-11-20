#pragma once

#include "workbench/color.h"
#include "workbench/tree.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace luciole::test {

  struct WorkbenchRow {
    int parent = -1;
    std::string_view name;
    bool folder = false;
    double bytes = 0;
    double items = 0;
    double files = 0;
    double modified = 0;
    double visits = 0;
    bool unreadable = false;
    bool truncated = false;
    bool otherFilesystem = false;
  };

  // Rows are listed parents first; children of a node keep their row order.
  inline workbench::Tree workbenchTree(const std::vector<WorkbenchRow>& rows, double now) {
    workbench::Tree tree;
    tree.now = now;
    tree.nodes.reserve(rows.size());
    for (const auto& row : rows) {
      workbench::Node node;
      node.name = std::string(row.name);
      node.parent = row.parent < 0 ? workbench::kNoNode : static_cast<workbench::NodeId>(row.parent);
      node.bytes = row.bytes;
      node.items = row.items;
      node.files = row.files;
      node.modified = row.modified;
      node.visits = row.visits;
      node.folder = row.folder;
      node.unreadable = row.unreadable;
      node.truncated = row.truncated;
      node.otherFilesystem = row.otherFilesystem;
      node.hidden = row.parent >= 0 && node.name.starts_with('.');
      node.category = static_cast<std::uint8_t>(workbench::fileCategory(node.name, node.folder));
      tree.nodes.push_back(std::move(node));
    }
    for (std::size_t parent = 0; parent < tree.nodes.size(); ++parent) {
      auto& node = tree.nodes[parent];
      node.firstChild = static_cast<std::uint32_t>(tree.children.size());
      for (std::size_t child = 0; child < rows.size(); ++child) {
        if (rows[child].parent == static_cast<int>(parent)) {
          tree.children.push_back(static_cast<workbench::NodeId>(child));
        }
      }
      node.childCount = static_cast<std::uint32_t>(tree.children.size()) - node.firstChild;
    }
    tree.root = tree.nodes.empty() ? workbench::kNoNode : 0;
    return tree;
  }

  // Ids: 0 r, 1 a, 2 b.txt, 3 .hidden, 4 c, 5 x, 6 y.png, 7 deep, 8 f.log, 9 g.c.
  inline workbench::Tree referenceTree() {
    return workbenchTree(
        {
            {.parent = -1,
             .name = "r",
             .folder = true,
             .bytes = 1000,
             .items = 4,
             .files = 5,
             .modified = 5000,
             .visits = 7},
            {.parent = 0,
             .name = "a",
             .folder = true,
             .bytes = 600,
             .items = 2,
             .files = 3,
             .modified = 4000,
             .visits = 5},
            {.parent = 0, .name = "b.txt", .bytes = 300, .modified = 2000, .visits = 2},
            {.parent = 0, .name = ".hidden", .bytes = 100, .modified = 1000},
            {.parent = 0, .name = "c", .folder = true, .modified = 500},
            {.parent = 1,
             .name = "x",
             .folder = true,
             .bytes = 500,
             .items = 2,
             .files = 2,
             .modified = 4000,
             .visits = 5},
            {.parent = 1, .name = "y.png", .bytes = 100, .modified = 3000},
            {.parent = 5,
             .name = "deep",
             .folder = true,
             .bytes = 400,
             .items = 1,
             .files = 1,
             .modified = 4000,
             .visits = 5},
            {.parent = 5, .name = "f.log", .bytes = 100, .modified = 3500},
            {.parent = 7, .name = "g.c", .bytes = 400, .modified = 4000, .visits = 5},
        },
        6000
    );
  }

  inline std::vector<std::string>
  workbenchNames(const workbench::Tree& tree, const std::vector<workbench::NodeId>& ids) {
    std::vector<std::string> names;
    names.reserve(ids.size());
    for (const auto id : ids) {
      names.push_back(tree.nodes[id].name);
    }
    return names;
  }

} // namespace luciole::test
