#include "fs/subtree_measure_walk.h"

#include "fs/hard_links.h"
#include "fs/native_listing.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <string_view>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace {
  using luciole::kNoMeasureRecord;
  using luciole::MeasureFile;
  using luciole::MeasureTotals;

  constexpr std::uint64_t kCheckEvery = 64;
  // Keeps record indices below kNoMeasureRecord: a record needs at least one examined entry.
  constexpr std::uint64_t kMaxEntries = 0xFFFFFFF0U;
  constexpr unsigned kStatxMask =
      STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_MTIME;

  std::uint64_t add(std::uint64_t a, std::uint64_t b) {
    return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
  }

  std::uint64_t blockBytes(std::uint64_t blocks) {
    constexpr std::uint64_t kBlock = 512;
    return blocks > std::numeric_limits<std::uint64_t>::max() / kBlock ? std::numeric_limits<std::uint64_t>::max()
                                                                       : blocks * kBlock;
  }

  // Adds what lies below; direct items stay with their own directory.
  void addBelow(MeasureTotals& into, const MeasureTotals& from) {
    into.apparentBytes = add(into.apparentBytes, from.apparentBytes);
    into.allocatedBytes = add(into.allocatedBytes, from.allocatedBytes);
    into.files = add(into.files, from.files);
    into.newest = std::max(into.newest, from.newest);
  }

  luciole::DirectoryStamp stampOf(const struct stat& info) {
    return {info.st_dev,          info.st_ino,         info.st_mtim.tv_sec,
            info.st_mtim.tv_nsec, info.st_ctim.tv_sec, info.st_ctim.tv_nsec};
  }

  luciole::MeasureTable failure(luciole::FileError error) { return {.records = {}, .error = std::move(error)}; }

  // What a file adds to the totals of the folders above it.
  MeasureTotals totalsOf(const MeasureFile& file) {
    return {
        .apparentBytes = file.apparentBytes,
        .allocatedBytes = file.allocatedBytes,
        .files = file.repeatedLink ? 0U : 1U,
        .newest = file.modified
    };
  }

  // More bytes first, then by name.
  bool larger(const MeasureFile& a, const MeasureFile& b) {
    return a.apparentBytes != b.apparentBytes ? a.apparentBytes > b.apparentBytes : a.name < b.name;
  }

  enum class Stop : std::uint8_t { None, Limit, Cancelled };

  struct Pending {
    std::string name;
    std::uint32_t record = kNoMeasureRecord;
    MeasureTotals own; // the directory's own blocks and modification time
  };

  struct Frame {
    Frame(luciole::Fd descriptor, std::uint32_t index, std::uint32_t level, const MeasureTotals& own)
        : fd(std::move(descriptor)), record(index), depth(level), totals(own) {}
    luciole::Fd fd;
    std::uint32_t record = kNoMeasureRecord;
    std::uint32_t depth = 0;
    MeasureTotals totals; // own entries and finished children so far
    std::vector<Pending> pending;
    std::size_t next = 0;
    std::vector<MeasureFile> largest; // a heap whose front is the smallest kept file
    bool truncated = false;
  };

  class Walker {
  public:
    Walker(const luciole::MeasureOptions& options, const luciole::MeasureProgress& progress)
        : m_options(options), m_progress(progress), m_budget(std::min<std::uint64_t>(options.entryBudget, kMaxEntries)),
          m_hardLinks(options.hardLinkBound) {}

    luciole::MeasureTable run(const luciole::Location& root) {
      const auto path = root.isNative() && !root.isTrash() ? root.nativePath() : std::nullopt;
      if (!path) {
        return failure({luciole::FileErrorKind::Unsupported, "Only local folders can be measured."});
      }
      luciole::Fd fd(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      struct stat info{};
      struct statfs filesystem{};
      if (!fd.valid() || ::fstat(fd.get(), &info) != 0 || ::fstatfs(fd.get(), &filesystem) != 0) {
        return failure(luciole::errnoError(errno));
      }
      if (luciole::remoteFilesystem(filesystem)) {
        return failure({luciole::FileErrorKind::Unsupported, "Remote folders are not measured."});
      }
      m_rootDevice = info.st_dev;
      m_directories.insert(info.st_ino);
      createRecord(root.basename().value_or("/"), kNoMeasureRecord, 0, false);
      m_table.records[0].flags.hidden = false;
      m_table.records[0].stamp = stampOf(info);
      m_lastProgress = std::chrono::steady_clock::now();
      m_stack.emplace_back(
          std::move(fd), 0, 0,
          MeasureTotals{.allocatedBytes = blockBytes(info.st_blocks), .newest = info.st_mtim.tv_sec}
      );
      read();
      // Each pass enters one pending directory or finishes one frame, so passes are bounded by the directories read.
      while (!m_stack.empty() && m_stop == Stop::None) {
        const Frame& top = m_stack.back();
        if (top.next < top.pending.size()) {
          if (m_table.entries >= m_budget) {
            m_stop = Stop::Limit;
            break;
          }
          enter();
        } else {
          finish(false);
          poll();
        }
      }
      unwind();
      m_table.hardLinksOverflow = m_hardLinks.overflow();
      m_table.records.shrink_to_fit(); // no room is kept for the records that were folded away
      return std::move(m_table);
    }

  private:
    std::uint32_t createRecord(const std::string& name, std::uint32_t parent, std::uint32_t depth, bool other) {
      const auto index = static_cast<std::uint32_t>(m_table.records.size());
      luciole::MeasureRecord record;
      record.name = name;
      record.parent = parent;
      record.depth = depth;
      record.flags = {.partial = !other, .otherFilesystem = other, .truncated = other, .hidden = name.starts_with('.')};
      m_table.records.push_back(std::move(record));
      m_ranges.emplace_back(0, 0);
      if (parent != kNoMeasureRecord) {
        ++m_table.records[parent].childCount;
      }
      return index;
    }

    // Cancellation, the deadline and partial tables; called every kCheckEvery entries and per finished directory.
    void poll() {
      const auto now = std::chrono::steady_clock::now();
      if (m_options.cancel.cancelled() || now >= m_options.deadline) {
        m_stop = m_options.cancel.cancelled() ? Stop::Cancelled : Stop::Limit;
        return;
      }
      if (m_progress && !m_stack.empty() && now - m_lastProgress >= m_options.progressInterval) {
        m_lastProgress = now;
        m_progress(partialTable());
      }
    }

    // Entries that could not be read are left out of the totals, so the directory and those above it are lower bounds.
    void markUnreadable(Frame& frame) {
      ++m_table.unreadable;
      frame.truncated = true;
      if (frame.record != kNoMeasureRecord) {
        m_table.records[frame.record].flags.unreadable = true;
      }
    }

    // Reads the top frame's entries. Bounded by the directory's size and by the entry budget.
    void read() {
      Frame& frame = m_stack.back();
      if (frame.record != kNoMeasureRecord) {
        auto& record = m_table.records[frame.record];
        record.firstChild = static_cast<std::uint32_t>(m_table.records.size());
        m_ranges[frame.record].first = record.firstChild;
      }
      const int descriptor = ::fcntl(frame.fd.get(), F_DUPFD_CLOEXEC, 0); // a listing needs no search permission
      const std::unique_ptr<DIR, void (*)(DIR*)> directory(
          descriptor >= 0 ? ::fdopendir(descriptor) : nullptr, [](DIR* value) { ::closedir(value); }
      );
      if (!directory) {
        if (descriptor >= 0) {
          ::close(descriptor);
        }
        markUnreadable(frame);
        return;
      }
      bool failed = false;
      while (m_stop == Stop::None) {
        errno = 0;
        const dirent* item = ::readdir(directory.get());
        if (item == nullptr) {
          failed = failed || errno != 0;
          break;
        }
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) {
          continue;
        }
        if (m_table.entries >= m_budget) {
          m_stop = Stop::Limit;
          break;
        }
        ++m_table.entries;
        if (m_table.entries % kCheckEvery == 0) {
          poll();
        }
        struct statx info{};
        const int flags = AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC;
        if (::statx(::dirfd(directory.get()), item->d_name, flags, kStatxMask, &info) != 0) {
          failed = failed || errno != ENOENT;
          frame.totals.items += errno != ENOENT ? 1 : 0;
          continue;
        }
        ++frame.totals.items;
        if ((info.stx_mask & STATX_TYPE) != 0 && S_ISDIR(info.stx_mode)) {
          addDirectory(frame, item->d_name, info);
        } else {
          addFile(frame, item->d_name, info);
        }
      }
      if (failed) {
        markUnreadable(frame);
      }
      if (frame.record != kNoMeasureRecord) {
        std::ranges::sort(frame.largest, larger);
        m_table.records[frame.record].files =
            std::make_shared<const std::vector<MeasureFile>>(std::move(frame.largest));
      }
    }

    void addDirectory(Frame& frame, const char* name, const struct statx& info) {
      const auto device = makedev(info.stx_dev_major, info.stx_dev_minor);
      const bool automount =
          (info.stx_attributes_mask & STATX_ATTR_AUTOMOUNT) != 0 && (info.stx_attributes & STATX_ATTR_AUTOMOUNT) != 0;
      const bool other = device != m_rootDevice || automount;
      if (!other && !m_directories.insert(info.stx_ino).second) {
        return; // Another name of a directory already counted.
      }
      const std::uint32_t depth = frame.depth + 1;
      if (frame.record != kNoMeasureRecord && depth > m_options.detailDepth) {
        m_table.records[frame.record].flags.foldersOmitted = true;
      }
      const auto record = frame.record != kNoMeasureRecord && depth <= m_options.detailDepth
          ? createRecord(name, frame.record, depth, other)
          : kNoMeasureRecord;
      if (other) {
        ++m_table.otherFilesystems;
        frame.truncated = true;
        return;
      }
      frame.pending.push_back({
          .name = name,
          .record = record,
          .own = {.allocatedBytes = blockBytes(info.stx_blocks), .newest = info.stx_mtime.tv_sec},
      });
    }

    void addFile(Frame& frame, std::string_view name, const struct statx& info) {
      const bool counted = info.stx_nlink <= 1 || m_hardLinks.first(info.stx_ino);
      const bool sized = S_ISREG(info.stx_mode) || S_ISLNK(info.stx_mode);
      MeasureFile file{
          .name = std::string(name),
          .apparentBytes = counted && sized ? info.stx_size : 0,
          .allocatedBytes = counted ? blockBytes(info.stx_blocks) : 0,
          .modified = info.stx_mtime.tv_sec,
          .hidden = name.starts_with('.'),
          .symlink = S_ISLNK(info.stx_mode),
          .repeatedLink = !counted,
      };
      addBelow(frame.totals, totalsOf(file));
      if (frame.record == kNoMeasureRecord) {
        return;
      }
      auto& heap = frame.largest;
      if (heap.size() < m_options.fileCap) {
        heap.push_back(std::move(file));
        std::ranges::push_heap(heap, larger);
        return;
      }
      if (!heap.empty() && larger(file, heap.front())) {
        std::ranges::pop_heap(heap, larger);
        std::swap(heap.back(), file);
        std::ranges::push_heap(heap, larger);
      }
      auto& rest = m_table.records[frame.record].otherFiles;
      ++rest.count;
      addBelow(rest.totals, totalsOf(file));
    }

    void enter() {
      Frame& parent = m_stack.back();
      Pending child = std::move(parent.pending[parent.next++]);
      const std::uint32_t depth = parent.depth + 1;
      const bool tooDeep = depth > m_options.maxDepth;
      luciole::Fd fd(
          tooDeep ? -1 : ::openat(parent.fd.get(), child.name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
      );
      if (!fd.valid()) {
        addBelow(parent.totals, child.own);
        parent.truncated = true;
        m_table.truncated |= tooDeep;
        m_table.unreadable += tooDeep ? 0 : 1;
        if (child.record != kNoMeasureRecord) {
          auto& record = m_table.records[child.record];
          record.totals = child.own;
          record.flags.partial = false;
          record.flags.truncated = true;
          record.flags.unreadable = !tooDeep;
        }
        return;
      }
      if (struct stat info{}; child.record != kNoMeasureRecord && ::fstat(fd.get(), &info) == 0) {
        m_table.records[child.record].stamp = stampOf(info);
      }
      m_stack.emplace_back(std::move(fd), child.record, depth, child.own);
      read();
    }

    void finish(bool keepPartial) {
      const Frame frame = std::move(m_stack.back());
      m_stack.pop_back();
      if (frame.record != kNoMeasureRecord) {
        auto& record = m_table.records[frame.record];
        record.totals = frame.totals;
        record.flags.truncated |= frame.truncated;
        record.flags.partial = keepPartial;
        if (record.childCount > m_options.childCap) {
          fold(frame.record);
        }
        m_ranges[frame.record].second = static_cast<std::uint32_t>(m_table.records.size());
      }
      if (!m_stack.empty()) {
        addBelow(m_stack.back().totals, frame.totals);
        m_stack.back().truncated |= frame.truncated;
      }
    }

    // Keeps the largest child records of a finished record and folds the rest with their descendants: a rewrite
    // of the table's tail, since every record after the children was made while walking them.
    void fold(std::uint32_t parent) {
      auto& records = m_table.records;
      const std::uint32_t first = records[parent].firstChild;
      const std::uint32_t count = records[parent].childCount;
      std::vector<std::uint32_t> order(count);
      std::ranges::iota(order, first);
      std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        const auto& left = records[a];
        const auto& right = records[b];
        return left.totals.apparentBytes != right.totals.apparentBytes
            ? left.totals.apparentBytes > right.totals.apparentBytes
            : left.name < right.name;
      });
      std::vector<bool> keep(count, false);
      auto& rest = records[parent].otherDirectories;
      std::vector<std::uint32_t> named; // the folded children that get an entry, largest first
      for (std::size_t i = 0; i < order.size(); ++i) {
        if (i < m_options.childCap) {
          keep[order[i] - first] = true;
        } else {
          ++rest.count;
          addBelow(rest.totals, records[order[i]].totals);
          rest.totals.items = add(rest.totals.items, records[order[i]].totals.items);
          if (named.size() < m_options.foldedCap) {
            named.push_back(order[i]);
          }
        }
      }
      records[parent].folded = luciole::foldedChildren(records, named, rest.count > named.size());
      std::vector<std::uint32_t> remap(records.size() - first, kNoMeasureRecord);
      std::vector<luciole::MeasureRecord> tail;
      const auto take = [&](std::uint32_t index) {
        remap[index - first] = first + static_cast<std::uint32_t>(tail.size());
        tail.push_back(std::move(records[index]));
      };
      for (std::uint32_t c = 0; c < count; ++c) {
        if (keep[c]) {
          take(first + c);
        }
      }
      for (std::uint32_t c = 0; c < count; ++c) {
        for (auto index = m_ranges[first + c].first; keep[c] && index < m_ranges[first + c].second; ++index) {
          take(index);
        }
      }
      for (auto& record : tail) {
        if (record.parent != parent) {
          record.parent = remap[record.parent - first];
        }
        if (record.childCount > 0) {
          record.firstChild = remap[record.firstChild - first];
        }
      }
      records.resize(first);
      std::ranges::move(tail, std::back_inserter(records));
      m_ranges.resize(records.size());
      records[parent].childCount = static_cast<std::uint32_t>(std::ranges::count(keep, true));
    }

    // A copy whose open directories carry what was counted below them so far, and whose directory being read
    // shows the files it kept so far.
    luciole::MeasureTable partialTable() const {
      luciole::MeasureTable table = m_table;
      table.partial = true;
      MeasureTotals carry;
      for (const auto& frame : m_stack | std::views::reverse) {
        MeasureTotals current = frame.totals;
        addBelow(current, carry);
        if (frame.record != kNoMeasureRecord) {
          auto& record = table.records[frame.record];
          record.totals = current;
          if (!record.files) {
            auto kept = frame.largest;
            std::ranges::sort(kept, larger);
            record.files = std::make_shared<const std::vector<MeasureFile>>(std::move(kept));
          }
        }
        carry = current;
      }
      return table;
    }

    // Closes the frames a stop left open. A limit marks them truncated; a cancellation leaves them partial.
    void unwind() {
      const bool cancelled = m_stop == Stop::Cancelled;
      m_table.cancelled = cancelled;
      m_table.truncated |= m_stop == Stop::Limit;
      while (!m_stack.empty()) {
        Frame& top = m_stack.back();
        for (auto i = top.next; i < top.pending.size(); ++i) {
          const auto& child = top.pending[i];
          addBelow(top.totals, child.own);
          if (child.record != kNoMeasureRecord) {
            auto& record = m_table.records[child.record];
            record.totals = child.own;
            record.flags.truncated = !cancelled;
            record.flags.notEntered = !cancelled;
            record.flags.partial = cancelled;
          }
        }
        top.next = top.pending.size();
        top.truncated |= !cancelled;
        finish(cancelled);
      }
    }

    const luciole::MeasureOptions& m_options;
    const luciole::MeasureProgress& m_progress;
    std::uint64_t m_budget;
    luciole::MeasureTable m_table;
    std::vector<Frame> m_stack;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> m_ranges; // descendants of each record, as indices
    std::unordered_set<std::uint64_t> m_directories;
    luciole::HardLinkSet m_hardLinks;
    dev_t m_rootDevice = 0;
    std::chrono::steady_clock::time_point m_lastProgress;
    Stop m_stop = Stop::None;
  };
} // namespace

luciole::MeasureTable
luciole::measureSubtree(const Location& root, const MeasureOptions& options, const MeasureProgress& progress) {
  return Walker(options, progress).run(root);
}

std::optional<luciole::DirectoryStamp> luciole::directoryStamp(const Location& folder) {
  const auto path = folder.isNative() ? folder.nativePath() : std::nullopt;
  struct stat info{};
  if (!path || ::stat(path->c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    return std::nullopt;
  }
  return stampOf(info);
}
