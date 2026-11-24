#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

namespace luciole {
  // A hard-linked file a walk counted at its first name.
  struct HardLinkEntry {
    std::uint64_t inode = 0;
    std::uint64_t apparentBytes = 0;
    std::uint64_t allocatedBytes = 0;
    bool direct = false; // in the walk's root folder itself
  };

  // The counted hard-linked files of a finished walk, sorted by inode, at most `bound` of them.
  struct HardLinks {
    std::vector<HardLinkEntry> entries;
    std::size_t bound = 0;
  };

  // The inodes of hard-linked files a walk counted already. At most `bound` are kept; past it a later name of a
  // file not kept counts again, and `overflow()` says so.
  class HardLinkSet {
  public:
    explicit HardLinkSet(std::size_t bound) : m_bound(bound) {}

    // False for a later name of a file seen before; true for its first name, and past the bound.
    bool first(const HardLinkEntry& entry);
    bool overflow() const { return m_overflow; }
    // What was kept, sorted.
    std::shared_ptr<const HardLinks> take();

  private:
    std::size_t m_bound;
    std::unordered_set<std::uint64_t> m_inodes;
    std::vector<HardLinkEntry> m_entries;
    bool m_overflow = false;
  };
} // namespace luciole
