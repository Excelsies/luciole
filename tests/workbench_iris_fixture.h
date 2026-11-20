#pragma once

#include "workbench/iris_scene.h"
#include "workbench/tree.h"
#include "workbench_fixture.h"

#include <format>
#include <limits>
#include <string>
#include <vector>

namespace luciole::test::iris {
  using namespace workbench;
  using namespace workbench::iris;

  inline constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

  // The reference fixture opened at x (ids: 1 a, 5 x, 7 deep, 8 f.log), below a and r.
  inline Tree folderX() {
    auto tree = referenceTree();
    tree.root = 5;
    tree.ancestors = {1, 0};
    return tree;
  }

  inline SceneInput hostInput(double width, double height, bool compact) {
    SceneInput input;
    input.width = width;
    input.height = height;
    input.compact = compact;
    return input;
  }

  inline SceneInput fullInput() { return hostInput(1400, 737, false); }

  // A root folder of 60 files named with `prefix`, the first heaviest.
  struct FileFolder {
    std::vector<std::string> names;
    Tree tree;
  };

  inline FileFolder fileFolder(const std::string& prefix) {
    FileFolder folder;
    for (int i = 0; i < 60; ++i) {
      folder.names.push_back(std::format("{}{:02}.txt", prefix, i));
    }
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .bytes = 1830000, .items = 60}};
    for (int i = 0; i < 60; ++i) {
      rows.push_back({.parent = 0, .name = folder.names[static_cast<std::size_t>(i)], .bytes = (60.0 - i) * 1000});
    }
    folder.tree = luciole::test::workbenchTree(rows, 0);
    return folder;
  }
} // namespace luciole::test::iris
