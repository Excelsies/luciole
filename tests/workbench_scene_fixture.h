#pragma once

#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::test {
  // A piece as the shape tests name it: its node and role, and the column of a files bar.
  struct SceneKey {
    workbench::NodeId node = workbench::kNoNode;
    workbench::SceneRole role = workbench::SceneRole::Piece;
    std::size_t part = 0;
    bool operator==(const SceneKey&) const = default;
  };

  inline SceneKey sceneKeyOf(const workbench::Shape& shape) {
    const auto role = workbench::sceneRole(shape.id.role);
    return {
        .node = shape.id.node,
        .role = role,
        .part = role == workbench::SceneRole::Files ? shape.id.role & 0xFFU : 0U,
    };
  }

  inline const workbench::Shape* findSceneShape(const workbench::SceneShapes& out, SceneKey key) {
    const auto found =
        std::ranges::find_if(out.shapes, [&key](const workbench::Shape& shape) { return sceneKeyOf(shape) == key; });
    return found == out.shapes.end() ? nullptr : &*found;
  }

  // Shapes of a piece that draw, in list order: they take no hits.
  inline std::vector<const workbench::Shape*> drawn(const workbench::SceneShapes& out, SceneKey key) {
    std::vector<const workbench::Shape*> found;
    for (const auto& shape : out.shapes) {
      if (!shape.hittable && sceneKeyOf(shape) == key) {
        found.push_back(&shape);
      }
    }
    return found;
  }

  inline const workbench::Shape* firstDrawn(const workbench::SceneShapes& out, SceneKey key) {
    const auto all = drawn(out, key);
    return all.empty() ? nullptr : all.front();
  }

  inline const workbench::Shape* hitTarget(const workbench::SceneShapes& out, SceneKey key) {
    const auto found = std::ranges::find_if(out.shapes, [&key](const workbench::Shape& shape) {
      return shape.hittable && sceneKeyOf(shape) == key;
    });
    return found == out.shapes.end() ? nullptr : &*found;
  }

  inline bool sameRect(const workbench::ShapeRect& rect, double x, double y, double w, double h) {
    return std::abs(rect.x - x) < 1e-3
        && std::abs(rect.y - y) < 1e-3
        && std::abs(rect.width - w) < 1e-3
        && std::abs(rect.height - h) < 1e-3;
  }

  inline bool sameRgb(workbench::Rgba8 color, workbench::Rgb rgb) {
    return color.r == rgb.r && color.g == rgb.g && color.b == rgb.b;
  }

  // top > home > user > work, each level with siblings; work holds 8 folders of three entries and 16 files, and its
  // second entry is still being measured.
  struct SceneFixture {
    workbench::Tree tree;
    workbench::NodeId work = workbench::kNoNode;
    workbench::NodeId partial = workbench::kNoNode;
  };

  inline SceneFixture sceneFixture() {
    std::vector<WorkbenchRow> rows;
    std::vector<std::string> names; // owns the row names
    names.reserve(200);
    const auto add = [&](int parent, std::string name, bool folder, double bytes, double items) {
      names.push_back(std::move(name));
      rows.push_back(
          {.parent = parent, .name = names.back(), .folder = folder, .bytes = bytes, .items = items, .files = items}
      );
      return static_cast<int>(rows.size()) - 1;
    };
    const int top = add(-1, "top", true, 9e6, 5);
    const int home = add(top, "home", true, 5e6, 4);
    for (const auto* name : {"etc", "var", "opt"}) {
      add(top, name, true, 1e6, 2);
    }
    add(top, "x.txt", false, 2e5, 0);
    const int user = add(home, "user", true, 4e6, 5);
    add(home, "guest", true, 4e5, 1);
    add(home, "shared", true, 3e5, 1);
    add(home, "notes.md", false, 1e5, 0);
    const int work = add(user, "work", true, 3e6, 24);
    for (const auto* name : {"music", "pics", "todo.txt"}) {
      add(user, name, std::string_view(name) != "todo.txt", 3e5, 2);
    }
    for (int i = 0; i < 8; ++i) {
      const int folder = add(work, std::format("p{}", i), true, 2e5 * (8 - i), 3);
      add(folder, std::format("p{}a", i), true, 1e5 * (8 - i), 1);
      add(folder, std::format("p{}b.png", i), false, 6e4 * (8 - i), 0);
      add(folder, std::format("p{}c.c", i), false, 4e4 * (8 - i), 0);
    }
    for (int i = 0; i < 16; ++i) {
      add(work, std::format("f{:02}.txt", i), false, 9e4 / (i + 1), 0);
    }
    SceneFixture fx;
    fx.tree = workbenchTree(rows, 1e6);
    fx.work = static_cast<workbench::NodeId>(work);
    fx.tree.root = fx.work;
    fx.tree.ancestors = workbench::parentChain(fx.tree, fx.work);
    fx.partial = fx.tree.children[fx.tree.nodes[fx.work].firstChild + 1];
    fx.tree.nodes[fx.partial].partial = true;
    return fx;
  }

  // One folder of 300 small files of seven sizes.
  inline workbench::Tree manyFilesTree() {
    std::vector<WorkbenchRow> rows{{.parent = -1, .name = "r", .folder = true, .bytes = 3e5, .items = 300}};
    std::vector<std::string> names(300);
    for (std::size_t i = 0; i < names.size(); ++i) {
      names[i] = std::format("file{:03}.txt", i);
      rows.push_back({.parent = 0, .name = names[i], .bytes = 1000.0 + static_cast<double>(i % 7) * 400});
    }
    return workbenchTree(rows, 0);
  }
} // namespace luciole::test
