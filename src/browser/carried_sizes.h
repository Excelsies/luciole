#pragma once

#include "browser/workbench_snapshot.h"
#include "fs/folded_children.h"
#include "workbench/tree.h"

#include <memory>

namespace luciole {
  // While the second pass of a folder runs, the folders its first walk never entered would read blank again. Gives each
  // of them, in `tree` and in `folded`, the size `previous` showed for the folder of that name, and moves the root's
  // size and the folded remainder by the difference. Folders `previous` showed nothing for stay as they are.
  void carryPreviousSizes(
      workbench::Tree& tree, std::shared_ptr<const FoldedChildren>& folded, const WorkbenchSnapshot& previous
  );
} // namespace luciole
