#pragma once

#include "fs/subtree_measure_walk.h"

#include <memory>
#include <span>
#include <string>

namespace luciole {
  // What the walk of one folder alone says of it: the root of its table, as a replacement for the record the walk of
  // its parent left for it when the budget ended that walk before the folder was entered.
  struct MeasureAmendment {
    std::string name;
    MeasureTotals totals;
    MeasureFlags flags; // truncated, unreadable and otherFilesystem as that walk ended
    std::shared_ptr<const std::vector<MeasureFile>> files;
    MeasureRemainder otherFiles;
    bool hasFolders = false; // folders lie inside: they have no records
  };

  // The amendment a finished table of the folder `name` gives; a table without records, that is, a folder that could
  // not be measured, gives an unreadable one with nothing counted.
  MeasureAmendment amendmentOf(std::string name, const MeasureTable& table);

  // A copy of `table` where each amendment replaces the never-entered child of the root it names, kept as a record or
  // only in the folded list, and the root's totals follow. Amendments that name no never-entered child are
  // ignored. Null when none applied, so the caller keeps `table`.
  std::shared_ptr<const MeasureTable>
  amendedTable(const MeasureTable& table, std::span<const MeasureAmendment> amendments);
} // namespace luciole
