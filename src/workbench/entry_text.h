#pragma once

#include "workbench/measure.h"
#include "workbench/tree.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// What a tooltip says of an entry, written once for every view: the facts, and the text they make.
namespace luciole::workbench {
  // The facts of an entry in the words every tooltip uses. A size or count that is not final carries its state
  // ("at least 3.0 GiB", "measuring…").
  struct EntryFacts {
    std::string name;                      // "/" for an unnamed top
    std::string size;                      // bytes
    std::string items;                     // a folder's item count; empty for a file
    std::string files;                     // a folder's file count; empty for a file
    std::string type;                      // a file's extension in capitals, or "file"; empty for a folder
    std::string modified;                  // "modified 3 h ago"
    std::string opens;                     // "5 opens"; empty when never opened
    std::string share;                     // of its siblings, "about " before it when not final; empty if not asked
    std::vector<std::string_view> reasons; // folderReasons
    bool hidden = false;
    std::string notCounted; // notCountedText; empty unless the size is a lower bound
  };

  // The facts of `id`, with its share among its siblings when `settings` is given. Empty facts for an unknown id.
  EntryFacts entryFacts(const Tree& tree, NodeId id, const LayoutSettings* settings = nullptr);

  // The tooltip text: the name, a line of facts joined by kFactsDot, and the sentence of notCounted on a line of its
  // own. "measuring…" and "not measured" are said once. Empty for facts without a name.
  std::string entryText(const EntryFacts& facts);

  // The tooltip text of a merged piece that stands for `nodes`: "N more entries", their summed size and why it reads
  // "at least". Empty unless that size is a lower bound; an amount that is exact or still measuring needs no reason.
  std::string mergedText(const Tree& tree, std::span<const NodeId> nodes);
} // namespace luciole::workbench
