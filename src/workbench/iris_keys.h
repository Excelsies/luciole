#pragma once

#include "workbench/iris_scene.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <optional>

// Keyboard walks over the label rows of a ring diagram. Every row is a stop, "+N more" included, and rows a
// compact dial does not show still serve the keyboard. Rows are named by their main ring sector.
namespace luciole::workbench::iris {
  enum class KeyMove : std::uint8_t { Down, Up, PageDown, PageUp, Home, End, Left, Right };

  // The sector of the row the cursor is on: "+N more" when `stop` says so, else the entry sector of `cursor`. None
  // when no row shows it, a cursor inside "+N more" included.
  std::optional<std::size_t> cursorRow(const Scene& scene, NodeId cursor, CursorStop stop);

  // Where `move` takes the cursor from the row of sector `from`. Down, Up and the pages stay in the column, a page
  // being half the rows that fit, and stop at its ends; Home and End go to the first and last row in angular order
  // (the right column top down, then the left one bottom up); Left and Right go to the row of that column nearest
  // in height. From no row, Down, Up and the pages go to the first row of the right column, else the left one;
  // Left and Right to the first row of that column. None for no rows, a move into the column the cursor is in, or
  // a `from` that names no row.
  std::optional<std::size_t> keyTarget(const Scene& scene, std::optional<std::size_t> from, KeyMove move);

  // Where the list of the main ring's "+N more" opens when the keyboard asks for it: the middle of its row when the
  // row shows, else the dial's center.
  ring::Point moreAnchor(const Scene& scene);
} // namespace luciole::workbench::iris
