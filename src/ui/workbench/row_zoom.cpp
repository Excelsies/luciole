#include "ui/workbench/row_zoom.h"

#include "render/scene/node.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/workbench_view.h"
#include "workbench/column_plan.h"

#include <cstddef>

namespace luciole {
  namespace {
    bool over(const Node& node, float x, float y) {
      for (const Node* up = &node; up != nullptr; up = up->parent()) {
        if (!up->visible()) {
          return false;
        }
      }
      return node.containsScenePoint(x, y);
    }
  } // namespace

  void setRowListIconSize(WorkbenchView& view, int size) {
    for (std::size_t slot = 0; slot < workbench::kColumnsAhead; ++slot) {
      view.column(slot).setListIconSize(size);
    }
    view.jumpList().setListIconSize(size);
  }

  bool overRowList(WorkbenchView& view, const Node* list, float x, float y) {
    if (list != nullptr && over(*list, x, y)) {
      return true;
    }
    for (std::size_t slot = 0; slot < workbench::kColumnsAhead; ++slot) {
      if (over(view.column(slot), x, y)) {
        return true;
      }
    }
    return over(view.jumpList(), x, y);
  }
} // namespace luciole
