#pragma once

#include "workbench/tree.h"

#include <cstddef>
#include <span>
#include <string>

// The sentence a tooltip gives for an amount that reads "at least": what the snapshot knows about the folders that
// were not counted.
namespace luciole::workbench {
  // Nodes read or waiting for one sentence. A walk that would pass it says only the general sentence.
  inline constexpr std::size_t kNotCountedVisitCap = 2000;

  // Why the size or counts of `id` are a lower bound, as "Not counted: proc, sys and dev are on another filesystem."
  // or "Not counted: 2 unreadable folders (cache, lost+found)." The folders are the ones at or below the node that the
  // snapshot flags, at most three named and then "and N more"; whatever the snapshot cannot place is told as "some
  // folders below were not counted in full", and so is everything when the walk reaches `cap` nodes. Empty unless
  // the node is a lower bound, and for an unknown id. A folder above the open one, in Tree::ancestors, is measured
  // under a limit its flags do not show: it gets one sentence saying so, with no folder named.
  std::string notCountedText(const Tree& tree, NodeId id, std::size_t cap = kNotCountedVisitCap);

  // The same for the sum of several nodes, such as the entries a "+N more" piece stands for: the nodes that are lower
  // bounds count, the others and unknown ids are skipped. Empty when none is one.
  std::string notCountedText(const Tree& tree, std::span<const NodeId> ids, std::size_t cap = kNotCountedVisitCap);
} // namespace luciole::workbench
