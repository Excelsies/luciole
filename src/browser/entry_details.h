#pragma once

#include "fs/directory_session.h"
#include "fs/folder_totals.h"
#include "model/directory_options.h"

#include <string_view>

namespace luciole {
  std::uint8_t entryColumnDetails(DetailsColumn column);
  std::string entryColumnText(const DirectoryEntry& entry, DetailsColumn column, std::string_view typeLabel = {});
  // Size cell text for a folder row; ≥ marks partial totals
  std::string entryFolderSizeText(const FolderTotalsSnapshot& totals);
  // A folder's size as the Size cell shows it: "≥ " first when the number is a lower bound.
  std::string folderSizeText(std::uint64_t bytes, bool lowerBound);
  std::string metadataTime(std::optional<std::uint64_t> seconds);
  std::optional<std::uint64_t> entrySortNumber(const DirectoryEntry& entry, SortField field);
} // namespace luciole
