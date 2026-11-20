#pragma once

#include "browser/measure_cache.h"
#include "workbench/tree.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  // One entry of a folder listing, used for a level the cache cannot describe yet.
  struct ListedEntry {
    std::string name;
    bool folder = false;
    bool hidden = false;
    std::uint64_t bytes = 0;                           // a file's size
    std::int64_t modified = 0;                         // seconds
    std::optional<std::uint64_t> items = std::nullopt; // a folder's direct entries, when known
  };

  struct MeasuredLevel {
    Location location;
    std::vector<ListedEntry> entries; // its listing; may be empty
  };

  struct MeasuredTreeInput {
    Location current;
    std::vector<ListedEntry> entries;              // the current folder's listing, used until a measure is known
    std::vector<MeasuredLevel> ancestors;          // nearest first; the chain stops at a level that is not the parent
    std::function<double(const Location&)> visits; // visit count of a folder or file; empty for none
    double now = 0;
    std::uint32_t levels = 3;       // levels below the current folder, at most 8
    std::size_t maxNodes = 200'000; // folders whose entries would pass this are flagged truncated
  };

  struct MeasuredTree {
    workbench::Tree tree;
    // Nodes standing for folded entries, ascending: one per folder for files beyond the file cap and one for
    // folders beyond the child cap. They have an empty name; items counts the folded entries and files the files
    // below them, a later name of a hard-linked file left out.
    std::vector<workbench::NodeId> remainders;
  };

  // The current folder with `levels` levels below it from the cache's view, and one node per ancestor listing its
  // entries. Levels the view does not know come from the listings, flagged partial, with folder sizes unknown.
  // Pure: reads the view and calls `visits`, nothing else, so a worker may run it.
  MeasuredTree buildMeasuredTree(const MeasuredTreeInput& input, const MeasureView& view);
} // namespace luciole
