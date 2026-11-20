#include "workbench/iris_keys.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace luciole::workbench::iris {
  namespace {
    struct Place {
      const std::vector<LabelRow>* column = nullptr;
      std::size_t row = 0;
    };

    std::optional<Place> placeOf(const Scene& scene, std::size_t sector) {
      for (const auto* column : {&scene.rows.right, &scene.rows.left}) {
        for (std::size_t i = 0; i < column->size(); ++i) {
          if ((*column)[i].sector == sector) {
            return Place{.column = column, .row = i};
          }
        }
      }
      return std::nullopt;
    }

    std::optional<std::size_t> firstOf(const std::vector<LabelRow>& column) {
      return column.empty() ? std::nullopt : std::optional(column.front().sector);
    }

    std::optional<std::size_t> walk(const Scene& scene, const std::optional<Place>& at, KeyMove move) {
      if (!at) {
        const auto first = firstOf(scene.rows.right);
        return first ? first : firstOf(scene.rows.left);
      }
      const bool forward = move == KeyMove::Down || move == KeyMove::PageDown;
      const bool paged = move == KeyMove::PageDown || move == KeyMove::PageUp;
      const std::size_t step = paged ? std::max<std::size_t>(1, scene.dial.rowsFit / 2) : 1;
      const auto& column = *at->column;
      const std::size_t last = column.size() - 1;
      const std::size_t row = forward ? std::min(last, at->row + step) : at->row - std::min(at->row, step);
      return column[row].sector;
    }

    std::optional<std::size_t> edge(const Scene& scene, bool last) {
      const auto& right = scene.rows.right;
      const auto& left = scene.rows.left;
      if (last) {
        return left.empty() ? (right.empty() ? std::nullopt : std::optional(right.back().sector))
                            : std::optional(left.front().sector);
      }
      return right.empty() ? (left.empty() ? std::nullopt : std::optional(left.back().sector))
                           : std::optional(right.front().sector);
    }

    std::optional<std::size_t> across(const Scene& scene, const std::optional<Place>& at, KeyMove move) {
      const auto& target = move == KeyMove::Left ? scene.rows.left : scene.rows.right;
      if (target.empty() || (at && at->column == &target)) {
        return std::nullopt;
      }
      if (!at) {
        return target.front().sector;
      }
      const double y = (*at->column)[at->row].y;
      return std::ranges::min_element(target, {}, [y](const LabelRow& row) { return std::abs(row.y - y); })->sector;
    }
  } // namespace

  std::optional<std::size_t> cursorRow(const Scene& scene, NodeId cursor, CursorStop stop) {
    for (const auto* column : {&scene.rows.right, &scene.rows.left}) {
      for (const auto& row : *column) {
        if (row.sector >= scene.main.sectors.size()) {
          continue;
        }
        const auto& sector = scene.main.sectors[row.sector];
        const bool here = stop == CursorStop::More ? sector.kind == SectorKind::More
                                                   : sector.kind == SectorKind::Entry && sector.node == cursor;
        if (here) {
          return row.sector;
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::size_t> keyTarget(const Scene& scene, std::optional<std::size_t> from, KeyMove move) {
    std::optional<Place> at;
    if (from) {
      at = placeOf(scene, *from);
      if (!at) {
        return std::nullopt;
      }
    }
    switch (move) {
    case KeyMove::Down:
    case KeyMove::Up:
    case KeyMove::PageDown:
    case KeyMove::PageUp:
      return walk(scene, at, move);
    case KeyMove::Home:
    case KeyMove::End:
      return edge(scene, move == KeyMove::End);
    case KeyMove::Left:
    case KeyMove::Right:
      return across(scene, at, move);
    }
    return std::nullopt;
  }

  ring::Point moreAnchor(const Scene& scene) {
    const auto row = cursorRow(scene, kNoNode, CursorStop::More);
    const auto place = row ? placeOf(scene, *row) : std::nullopt;
    if (!place || !(*place->column)[place->row].shown) {
      return scene.dial.center;
    }
    const auto& shown = (*place->column)[place->row];
    return {.x = shown.box.x + shown.box.width / 2, .y = shown.y};
  }
} // namespace luciole::workbench::iris
