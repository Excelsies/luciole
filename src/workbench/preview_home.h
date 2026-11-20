#pragma once

#include "model/directory_options.h"
#include "workbench/pane_layout.h"

#include <cstddef>
#include <cstdint>

namespace luciole::workbench {
  enum class PreviewHome : std::uint8_t { Column, JumpAhead, None };

  // Whether the Jump ahead pane is shown in `layout`: not collapsed by choice or by force, and not hidden by the
  // expanded instrument or the rail.
  bool jumpAheadOpen(const PaneLayout& layout);

  // Where a file previewed from column `slot` (0 is the first column ahead) is shown when `columns` columns are
  // drawn. `JumpAheadAlways` puts it in the place of Jump ahead whatever the room. The other choices put it in its
  // own column when that column is drawn; when it is not, `JumpAheadWhenNarrow` puts it in the place of Jump ahead
  // and `HiddenWhenNarrow` nowhere. With the Jump ahead pane not shown, "the place of Jump ahead" is nowhere too.
  PreviewHome previewHome(std::size_t slot, std::size_t columns, bool jumpOpen, WorkbenchPreviewPlace choice);
} // namespace luciole::workbench
