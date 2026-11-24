#include "fs/folded_children.h"

#include "fs/subtree_measure_walk.h"

#include <algorithm>

std::shared_ptr<const luciole::FoldedChildren> luciole::FoldedChildren::make(std::vector<Source> sources, bool capped) {
  std::ranges::sort(sources, {}, &Source::name);
  auto list = std::make_shared<FoldedChildren>();
  list->capped = capped;
  list->entries.reserve(sources.size());
  for (const auto& source : sources) {
    list->names += source.name;
    list->entries.push_back(
        {.apparentBytes = source.apparentBytes,
         .allocatedBytes = source.allocatedBytes,
         .nameEnd = static_cast<std::uint32_t>(list->names.size()),
         .flags = source.flags}
    );
  }
  list->names.shrink_to_fit();
  return list;
}

std::uint8_t luciole::foldedFlagsOf(const MeasureFlags& flags) {
  using F = FoldedChildren;
  return static_cast<std::uint8_t>(
      (flags.truncated ? F::kTruncated : 0)
      | (flags.notEntered ? F::kNotEntered : 0)
      | (flags.unreadable ? F::kUnreadable : 0)
      | (flags.otherFilesystem ? F::kOtherFilesystem : 0)
  );
}

std::shared_ptr<const luciole::FoldedChildren> luciole::foldedChildren(
    const std::vector<MeasureRecord>& records, const std::vector<std::uint32_t>& indexes, bool capped
) {
  using F = FoldedChildren;
  std::vector<F::Source> sources;
  sources.reserve(indexes.size());
  for (const auto index : indexes) {
    const auto& record = records[index];
    sources.push_back(
        {.name = record.name,
         .apparentBytes = record.totals.apparentBytes,
         .allocatedBytes = record.totals.allocatedBytes,
         .flags = foldedFlagsOf(record.flags)}
    );
  }
  return F::make(std::move(sources), capped);
}
