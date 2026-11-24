#include "fs/hard_links.h"

#include <algorithm>

bool luciole::HardLinkSet::first(const HardLinkEntry& entry) {
  if (m_inodes.contains(entry.inode)) {
    return false;
  }
  if (m_inodes.size() < m_bound) {
    m_inodes.insert(entry.inode);
    m_entries.push_back(entry);
  } else {
    m_overflow = true;
  }
  return true;
}

std::shared_ptr<const luciole::HardLinks> luciole::HardLinkSet::take() {
  std::ranges::sort(m_entries, {}, &HardLinkEntry::inode);
  m_entries.shrink_to_fit();
  return std::make_shared<const HardLinks>(HardLinks{.entries = std::move(m_entries), .bound = m_bound});
}
