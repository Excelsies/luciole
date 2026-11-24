#pragma once

#include "ui/browser/file_view.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace luciole {
  class WorkbenchSession;

  // Keeps the Size cells of the list's folder rows in step with a Workbench session's measure. The measure grows
  // while a folder is walked; a row is bound again only when its own cell text changed.
  class WorkbenchFolderSizes {
  public:
    struct Hooks {
      std::function<void(std::size_t)> rebind; // binds the list's row at this index again
      std::function<void()> frameNeeded;
    };

    WorkbenchFolderSizes(FileViewAdapter& list, Hooks hooks);
    ~WorkbenchFolderSizes();
    WorkbenchFolderSizes(const WorkbenchFolderSizes&) = delete;
    WorkbenchFolderSizes& operator=(const WorkbenchFolderSizes&) = delete;

    // The list's cells come from `session` until unmount(), which goes before the session does.
    void mount(const WorkbenchSession& session);
    void unmount();
    // Once a frame: binds again the rows whose cell text changed since the last call.
    void refresh();

  private:
    FileViewAdapter& m_list;
    Hooks m_hooks;
    const WorkbenchSession* m_session = nullptr;
    std::uint64_t m_serial = 0; // the measure the cells were last brought up to
  };
} // namespace luciole
