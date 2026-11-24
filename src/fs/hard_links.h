#pragma once

#include <cstddef>
#include <unordered_set>

namespace luciole {
  // The inodes of hard-linked files a walk counted already. At most `bound` are kept; past it a later name of a
  // file not kept counts again, and `overflow()` says so.
  class HardLinkSet {
  public:
    explicit HardLinkSet(std::size_t bound) : m_bound(bound) {}

    // False for a later name of a file seen before; true for its first name, and past the bound.
    bool first(unsigned long long inode);
    bool overflow() const { return m_overflow; }

  private:
    std::size_t m_bound;
    std::unordered_set<unsigned long long> m_inodes;
    bool m_overflow = false;
  };
} // namespace luciole
