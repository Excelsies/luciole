#include "fs/hard_links.h"

bool luciole::HardLinkSet::first(unsigned long long inode) {
  if (m_inodes.contains(inode)) {
    return false;
  }
  if (m_inodes.size() < m_bound) {
    m_inodes.insert(inode);
  } else {
    m_overflow = true;
  }
  return true;
}
