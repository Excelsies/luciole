#pragma once

#include "browser/action_registry.h"
#include "browser/directory_model.h"
#include "fs/location.h"

#include <functional>
#include <span>

namespace luciole {
  // What the Workbench does to open a file. `visited` records the visit; `launch` hands the file to its program;
  // `browseArchive` shows the archive browser, after the same refusal Browse Archive gives, and says whether it
  // started. All are required.
  struct WorkbenchOpenHooks {
    std::function<void(const Location&)> visited;
    std::function<void(const Location&)> launch;
    std::function<bool(const ArchiveFacts&)> browseArchive;
  };

  // The facts Open decides on for `location`: from the row of the first of `models` that lists it, the page's listing
  // and the columns ahead, else from the file itself.
  ArchiveFacts archiveFactsFor(const Location& location, std::span<const DirectoryModel* const> models);

  // Opens a file of the Workbench as Open does in the other views: one browsable archive shows the archive browser,
  // outside a file chooser; any other file goes to its program. The visit is recorded once the open goes ahead: a
  // refused or failed archive browser records none.
  void openWorkbenchFile(
      const Location& location, std::span<const DirectoryModel* const> models, bool chooser,
      const WorkbenchOpenHooks& hooks
  );
} // namespace luciole
