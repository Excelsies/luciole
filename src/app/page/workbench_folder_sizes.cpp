#include "app/page/workbench_folder_sizes.h"

#include "app/page/workbench_session.h"

#include <string>

namespace luciole {
  WorkbenchFolderSizes::WorkbenchFolderSizes(FileViewAdapter& list, Hooks hooks)
      : m_list(list), m_hooks(std::move(hooks)) {}

  WorkbenchFolderSizes::~WorkbenchFolderSizes() { unmount(); }

  void WorkbenchFolderSizes::mount(const WorkbenchSession& session) {
    m_session = &session;
    m_serial = 0;
    m_list.setFolderSizeText([this](const DirectoryEntry& entry) {
      return m_session != nullptr ? m_session->folderSizeText(entry) : std::string{};
    });
  }

  void WorkbenchFolderSizes::unmount() {
    m_list.setFolderSizeText({});
    m_session = nullptr;
  }

  void WorkbenchFolderSizes::refresh() {
    if (m_session == nullptr || m_session->sizesSerial() == m_serial) {
      return;
    }
    m_serial = m_session->sizesSerial();
    const auto stale = m_list.staleFolderSizes();
    for (const auto index : stale) {
      m_hooks.rebind(index);
    }
    if (!stale.empty()) {
      m_hooks.frameNeeded();
    }
  }
} // namespace luciole
