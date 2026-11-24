#include "fs/measure_amend.h"

#include <algorithm>
#include <unordered_map>

namespace {
  using luciole::FoldedChildren;
  using luciole::MeasureAmendment;
  using luciole::MeasureTotals;

  std::uint64_t plus(std::uint64_t a, std::uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
  std::uint64_t minus(std::uint64_t a, std::uint64_t b) { return b > a ? 0 : a - b; }

  // `total` with a child's measure `was` replaced by `now`: what lies below the child, not its direct entries.
  void replace(MeasureTotals& total, const MeasureTotals& was, const MeasureTotals& now) {
    total.apparentBytes = plus(minus(total.apparentBytes, was.apparentBytes), now.apparentBytes);
    total.allocatedBytes = plus(minus(total.allocatedBytes, was.allocatedBytes), now.allocatedBytes);
    total.files = plus(minus(total.files, was.files), now.files);
    total.newest = std::max(total.newest, now.newest);
  }
} // namespace

luciole::MeasureAmendment luciole::amendmentOf(std::string name, const MeasureTable& table) {
  MeasureAmendment amendment;
  amendment.name = std::move(name);
  if (table.error || table.records.empty()) {
    amendment.flags.truncated = true;
    amendment.flags.unreadable = true;
    return amendment;
  }
  const auto& root = table.records[0];
  amendment.totals = root.totals;
  amendment.flags.truncated = root.flags.truncated;
  amendment.flags.unreadable = root.flags.unreadable;
  amendment.flags.otherFilesystem = root.flags.otherFilesystem;
  amendment.files = root.files;
  amendment.otherFiles = root.otherFiles;
  amendment.hasFolders = root.childCount > 0 || root.otherDirectories.count > 0;
  return amendment;
}

std::shared_ptr<const luciole::MeasureTable>
luciole::amendedTable(const MeasureTable& table, std::span<const MeasureAmendment> amendments) {
  if (table.records.empty() || amendments.empty()) {
    return nullptr;
  }
  auto out = std::make_shared<MeasureTable>(table);
  auto& root = out->records[0];
  std::unordered_map<std::string_view, std::uint32_t> never; // the root's never-entered child records, by name
  for (auto c = root.firstChild; c < root.firstChild + root.childCount && c < out->records.size(); ++c) {
    if (out->records[c].flags.notEntered) {
      never.emplace(out->records[c].name, c);
    }
  }
  std::shared_ptr<FoldedChildren> list; // a copy, made at the first folded child amended
  bool changed = false;
  for (const auto& amendment : amendments) {
    if (const auto found = never.find(amendment.name); found != never.end()) {
      auto& record = out->records[found->second];
      replace(root.totals, record.totals, amendment.totals);
      record.totals = amendment.totals;
      record.flags.notEntered = false;
      record.flags.partial = false;
      record.flags.truncated = amendment.flags.truncated;
      record.flags.unreadable = amendment.flags.unreadable;
      record.flags.otherFilesystem = amendment.flags.otherFilesystem;
      record.flags.foldersOmitted = amendment.hasFolders;
      record.files = amendment.files;
      record.otherFiles = amendment.otherFiles;
      never.erase(found);
      changed = true;
    } else if (root.folded) {
      list = list ? list : std::make_shared<FoldedChildren>(*root.folded);
      const auto* shown = list->find(amendment.name);
      if (shown == nullptr || (shown->flags & FoldedChildren::kNotEntered) == 0) {
        continue;
      }
      auto& entry = list->entries[static_cast<std::size_t>(shown - list->entries.data())];
      MeasureTotals was{.apparentBytes = entry.apparentBytes, .allocatedBytes = entry.allocatedBytes};
      replace(root.totals, was, amendment.totals);
      replace(root.otherDirectories.totals, was, amendment.totals);
      entry.apparentBytes = amendment.totals.apparentBytes;
      entry.allocatedBytes = amendment.totals.allocatedBytes;
      MeasureFlags flags;
      flags.truncated = amendment.flags.truncated;
      flags.unreadable = amendment.flags.unreadable;
      flags.otherFilesystem = amendment.flags.otherFilesystem;
      entry.flags = foldedFlagsOf(flags);
      changed = true;
    }
  }
  if (!changed) {
    return nullptr;
  }
  if (list) {
    root.folded = std::move(list);
  }
  // The root is a lower bound while any child, kept or folded, still is.
  bool cut = root.flags.unreadable;
  for (auto c = root.firstChild; c < root.firstChild + root.childCount && !cut; ++c) {
    cut = out->records[c].flags.truncated;
  }
  if (root.folded) {
    cut = cut || std::ranges::any_of(root.folded->entries, [](const FoldedChildren::Entry& entry) {
            return (entry.flags & (FoldedChildren::kTruncated | FoldedChildren::kUnreadable)) != 0;
          });
  }
  root.flags.truncated = cut;
  return out;
}
