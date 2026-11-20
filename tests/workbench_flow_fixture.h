#pragma once

#include "workbench/flow_layout.h"
#include "workbench/flow_topology.h"
#include "workbench/tree.h"
#include "workbench_fixture.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::test {

  struct FlowSpec {
    int parent = -1;
    std::string name;
    bool folder = false;
    double bytes = 0;
    double items = 0;
    double files = 0;
  };

  inline workbench::Tree flowTree(const std::vector<FlowSpec>& specs, double now, double modified) {
    std::vector<WorkbenchRow> rows;
    rows.reserve(specs.size());
    for (const auto& spec : specs) {
      rows.push_back(
          {.parent = spec.parent,
           .name = spec.name,
           .folder = spec.folder,
           .bytes = spec.bytes,
           .items = spec.folder ? spec.items : 0,
           .files = spec.files,
           .modified = modified}
      );
    }
    return workbenchTree(rows, now);
  }

  // w holds 14 folders "d<i>" of falling size; each has 6 subfolders "e<j>", each of those has 4 subfolders "g<k>"
  // and 3 files. w also holds 3000 tiny folders and 2000 files that must never be drawn as folders.
  inline workbench::Tree wideTree() {
    std::vector<FlowSpec> specs{{.parent = -1, .name = "w", .folder = true}};
    const auto add = [&specs](int parent, std::string name, bool folder, double bytes) {
      specs.push_back({.parent = parent, .name = std::move(name), .folder = folder, .bytes = bytes, .items = 1});
      return static_cast<int>(specs.size()) - 1;
    };
    for (int i = 0; i < 14; ++i) {
      const int d = add(0, std::format("d{}", i), true, 1e6 / (i + 1));
      for (int j = 0; j < 6; ++j) {
        const int e = add(d, std::format("e{}", j), true, 1e5 / (j + 1));
        for (int k = 0; k < 4; ++k) {
          add(e, std::format("g{}", k), true, 1e4 / (k + 1));
        }
        for (int k = 0; k < 3; ++k) {
          add(e, std::format("f{}", k), false, 500);
        }
      }
      add(d, std::format("readme{}", i), false, 2e3);
    }
    for (int i = 0; i < 3000; ++i) {
      add(0, std::format("t{}", i), true, 10);
    }
    for (int i = 0; i < 2000; ++i) {
      add(0, std::format("file{}", i), false, 300);
    }
    return flowTree(specs, 2000, 1000);
  }

  inline workbench::NodeId childNamed(const workbench::Tree& tree, workbench::NodeId parent, std::string_view name) {
    for (const auto id : workbench::children(tree, parent)) {
      if (tree.nodes[id].name == name) {
        return id;
      }
    }
    return workbench::kNoNode;
  }

  inline workbench::FlowLayout placeWide(const workbench::Tree& wide, double width = 1120, double height = 295) {
    return workbench::placeFlow(workbench::buildFlowTopology(wide, 0, {}), width, height, 0);
  }

  // The wide tree seen from `current`, with `ancestors` above it.
  inline workbench::Tree
  wideFrom(const workbench::Tree& wide, workbench::NodeId current, std::vector<workbench::NodeId> ancestors) {
    auto tree = wide;
    tree.root = current;
    tree.ancestors = std::move(ancestors);
    return tree;
  }

} // namespace luciole::test
