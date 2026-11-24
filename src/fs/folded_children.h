#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  // The name and size of every child folder a record folded into its remainder, as the Size column needs them: found by
  // name, with no records below. Names are one string, entries sorted by name, so a child costs 24 bytes and its name.
  struct FoldedChildren {
    static constexpr std::uint8_t kTruncated = 1;  // entered and cut, or never entered: the size is a lower bound
    static constexpr std::uint8_t kNotEntered = 2; // listed, and the walk ended first
    static constexpr std::uint8_t kUnreadable = 4;
    static constexpr std::uint8_t kOtherFilesystem = 8; // not entered, another filesystem

    struct Entry {
      std::uint64_t apparentBytes = 0;
      std::uint64_t allocatedBytes = 0;
      std::uint32_t nameEnd = 0; // where its name ends in `names`
      std::uint8_t flags = 0;
      bool operator==(const Entry&) const = default;
    };

    std::string names;          // raw name bytes, one after another in the order of `entries`
    std::vector<Entry> entries; // sorted by name
    bool capped = false;        // more children were folded than MeasureOptions::foldedCap: the smallest are missing

    std::string_view nameOf(std::size_t index) const {
      const std::size_t begin = index == 0 ? 0 : entries[index - 1].nameEnd;
      return std::string_view(names).substr(begin, entries[index].nameEnd - begin);
    }
    const Entry* find(std::string_view name) const {
      std::size_t low = 0;
      std::size_t high = entries.size();
      while (low < high) { // halves the range each time
        const std::size_t middle = low + ((high - low) / 2);
        const auto here = nameOf(middle);
        if (here == name) {
          return &entries[middle];
        }
        if (here < name) {
          low = middle + 1;
        } else {
          high = middle;
        }
      }
      return nullptr;
    }
    bool operator==(const FoldedChildren&) const = default;

    // A child to list: its name must outlive the call.
    struct Source {
      std::string_view name;
      std::uint64_t apparentBytes = 0;
      std::uint64_t allocatedBytes = 0;
      std::uint8_t flags = 0;
    };
    // The list of `sources` in any order, sorted by name; null for none.
    static std::shared_ptr<const FoldedChildren> make(std::vector<Source> sources, bool capped);

    // What the list holds on the heap.
    std::size_t heapBytes() const { return (entries.capacity() * sizeof(Entry)) + names.capacity(); }
  };
} // namespace luciole
