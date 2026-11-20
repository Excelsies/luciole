#include "workbench/preview_home.h"

namespace luciole::workbench {
  bool jumpAheadOpen(const PaneLayout& layout) { return layout.rects.jump.has_value() && !layout.collapsed.jump; }

  PreviewHome previewHome(std::size_t slot, std::size_t columns, bool jumpOpen, WorkbenchPreviewPlace choice) {
    const auto jump = jumpOpen ? PreviewHome::JumpAhead : PreviewHome::None;
    switch (choice) {
    case WorkbenchPreviewPlace::JumpAheadAlways:
      return jump;
    case WorkbenchPreviewPlace::HiddenWhenNarrow:
      return slot < columns ? PreviewHome::Column : PreviewHome::None;
    case WorkbenchPreviewPlace::JumpAheadWhenNarrow:
      return slot < columns ? PreviewHome::Column : jump;
    }
    return PreviewHome::None; // a value outside the enumeration
  }
} // namespace luciole::workbench
