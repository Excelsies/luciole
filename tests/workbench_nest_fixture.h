#pragma once

#include "test_check.h"
#include "workbench/measure.h"
#include "workbench/nest_scene.h"
#include "workbench/tree.h"
#include "workbench/treemap.h"
#include "workbench_fixture.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::test::nest {
  using namespace workbench;

  inline constexpr double kEps = 1e-6;
  inline constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  inline constexpr double kInf = std::numeric_limits<double>::infinity();
  inline const LayoutSettings kSettings{};

  // A chain 12 levels deep; every level holds six siblings; the last level holds 400 files of varied size.
  struct Fixture {
    Tree tree;
    std::vector<NodeId> levels; // levels[i] is l<i>

    // Makes the folder `depth` levels below the top the current one.
    NodeId at(std::size_t depth) {
      tree.root = depth == 0 ? 0 : levels[depth - 1];
      tree.ancestors = parentChain(tree, tree.root);
      return tree.root;
    }

    NodeId find(std::string_view name) const {
      for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        if (tree.nodes[i].name == name) {
          return static_cast<NodeId>(i);
        }
      }
      return kNoNode;
    }
  };

  inline Fixture fixture() {
    struct Row {
      int parent;
      std::string name;
      bool folder;
      double bytes;
      double items;
    };
    std::vector<Row> rows{{.parent = -1, .name = "r", .folder = true, .bytes = 0, .items = 4}};
    std::vector<int> levelRows;
    int parent = 0;
    for (int i = 0; i < 12; ++i) {
      for (int j = 0; j < 6; ++j) {
        const bool folder = j % 2 == 1;
        rows.push_back(
            {.parent = parent,
             .name = std::format("s{}_{}", i, j),
             .folder = folder,
             .bytes = 1000.0 * (j + 2) * (i + 1),
             .items = folder ? 2.0 : 0.0}
        );
      }
      rows.push_back({.parent = parent, .name = std::format("l{}", i), .folder = true, .bytes = 90000, .items = 7});
      parent = static_cast<int>(rows.size()) - 1;
      levelRows.push_back(parent);
    }
    for (int i = 0; i < 400; ++i) {
      rows.push_back(
          {.parent = parent,
           .name = std::format("f{}.dat", i),
           .folder = false,
           .bytes = 1.0 + ((i * 7919) % 5000),
           .items = 0}
      );
    }
    std::vector<WorkbenchRow> table;
    table.reserve(rows.size());
    for (const auto& row : rows) {
      table.push_back(
          {.parent = row.parent,
           .name = row.name,
           .folder = row.folder,
           .bytes = row.bytes,
           .items = row.items,
           .modified = 1000}
      );
    }
    Fixture result{.tree = luciole::test::workbenchTree(table, 5000), .levels = {}};
    for (const auto row : levelRows) {
      result.levels.push_back(static_cast<NodeId>(row));
    }
    return result;
  }

  inline bool inside(const TreemapRect& r, double width, double height, double margin) {
    return r.x >= margin - kEps
        && r.y >= margin - kEps
        && r.x + r.w <= width - margin + kEps
        && r.y + r.h <= height - margin + kEps;
  }

  inline bool finiteRect(const TreemapRect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h);
  }

  inline std::vector<const NestPiece*> piecesOf(const NestScene& scene, NestPieceKind kind) {
    std::vector<const NestPiece*> out;
    for (const auto& piece : scene.pieces) {
      if (piece.kind == kind) {
        out.push_back(&piece);
      }
    }
    return out;
  }

  inline const NestPiece* morePiece(const NestScene& scene) {
    const auto more = piecesOf(scene, NestPieceKind::More);
    return more.empty() ? nullptr : more.front();
  }

  inline std::optional<std::size_t> pieceOfNode(const NestScene& scene, NodeId id) {
    for (std::size_t i = 0; i < scene.pieces.size(); ++i) {
      if (scene.pieces[i].node == id
          && (scene.pieces[i].kind == NestPieceKind::Tile || scene.pieces[i].kind == NestPieceKind::Segment)) {
        return i;
      }
    }
    return std::nullopt;
  }

  inline void checkRect(const TreemapRect& actual, double x, double y, double w, double h) {
    TEST_NEAR(actual.x, x, kEps);
    TEST_NEAR(actual.y, y, kEps);
    TEST_NEAR(actual.w, w, kEps);
    TEST_NEAR(actual.h, h, kEps);
  }
} // namespace luciole::test::nest
