#include "fs/measure_amend.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

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
  amendment.hardLinks = table.hardLinks;
  amendment.hasFolders = root.childCount > 0 || root.otherDirectories.count > 0;
  return amendment;
}

namespace {
  bool larger(const luciole::MeasureFile& a, const luciole::MeasureFile& b) {
    return a.apparentBytes != b.apparentBytes ? a.apparentBytes > b.apparentBytes : a.name < b.name;
  }

  // The files counted for the root so far: the inodes seen, and the entries kept, at most `bound`.
  struct Counted {
    explicit Counted(const luciole::MeasureTable& table) {
      if (table.hardLinks) {
        kept = table.hardLinks->entries;
        bound = table.hardLinks->bound;
      }
      for (const auto& entry : kept) {
        seen.insert(entry.inode);
      }
    }
    std::unordered_set<std::uint64_t> seen;
    std::vector<luciole::HardLinkEntry> kept;
    std::size_t bound = luciole::MeasureOptions{}.hardLinkBound;
    bool overflow = false;
  };

  // Takes out of the amendment the files `counted` has, and adds the others to it. Their sizes leave the totals and
  // the folder's own file pieces: the later name reads as a later name, as in one walk.
  void leaveOutCounted(MeasureAmendment& amendment, Counted& counted) {
    if (!amendment.hardLinks) {
      return;
    }
    std::shared_ptr<std::vector<luciole::MeasureFile>> files;
    for (auto entry : amendment.hardLinks->entries) {
      if (!counted.seen.contains(entry.inode)) {
        if (counted.seen.size() < counted.bound) {
          counted.seen.insert(entry.inode);
          entry.direct = false;
          counted.kept.push_back(entry);
        } else {
          counted.overflow = true;
        }
        continue;
      }
      auto& totals = amendment.totals;
      totals.apparentBytes = minus(totals.apparentBytes, entry.apparentBytes);
      totals.allocatedBytes = minus(totals.allocatedBytes, entry.allocatedBytes);
      totals.files = minus(totals.files, 1);
      if (!entry.direct) {
        continue;
      }
      if (!files && amendment.files) {
        files = std::make_shared<std::vector<luciole::MeasureFile>>(*amendment.files);
      }
      const auto listed = files ? std::ranges::find(*files, entry.inode, &luciole::MeasureFile::inode)
                                : std::vector<luciole::MeasureFile>::iterator{};
      if (files && listed != files->end()) {
        listed->apparentBytes = 0;
        listed->allocatedBytes = 0;
        listed->repeatedLink = true;
      } else {
        auto& rest = amendment.otherFiles.totals;
        rest.apparentBytes = minus(rest.apparentBytes, entry.apparentBytes);
        rest.allocatedBytes = minus(rest.allocatedBytes, entry.allocatedBytes);
        rest.files = minus(rest.files, 1);
      }
    }
    if (files) {
      std::ranges::sort(*files, larger);
      amendment.files = std::move(files);
    }
  }
} // namespace

std::shared_ptr<const luciole::MeasureTable>
luciole::amendedTable(const MeasureTable& table, std::span<const MeasureAmendment> amendments, bool more) {
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
  Counted counted(table);
  for (auto amendment : amendments) {
    if (const auto found = never.find(amendment.name); found != never.end()) {
      leaveOutCounted(amendment, counted);
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
      leaveOutCounted(amendment, counted);
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
  out->hardLinksOverflow = out->hardLinksOverflow || counted.overflow;
  out->hardLinks.reset();
  if (more) {
    std::ranges::sort(counted.kept, {}, &HardLinkEntry::inode);
    out->hardLinks =
        std::make_shared<const HardLinks>(HardLinks{.entries = std::move(counted.kept), .bound = counted.bound});
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
