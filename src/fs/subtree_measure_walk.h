#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace luciole {
  inline constexpr std::uint32_t kNoMeasureRecord = 0xFFFFFFFFU;

  // Identity and change times of a directory when it was read.
  struct DirectoryStamp {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0; // within the second
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0; // within the second
    bool operator==(const DirectoryStamp&) const = default;
  };

  // Everything below a directory. A hard-linked file adds bytes and a file at its first name only.
  // Sums saturate at the largest value instead of wrapping.
  struct MeasureTotals {
    std::uint64_t apparentBytes = 0;  // sizes of the files and symlinks below
    std::uint64_t allocatedBytes = 0; // blocks of every entry below and of the directory itself
    std::uint64_t files = 0;          // entries below that are not directories
    std::uint64_t items = 0;          // direct entries
    std::int64_t newest = 0;          // newest modification below, the directory's own included, in seconds
    bool operator==(const MeasureTotals&) const = default;
  };

  struct MeasureFlags {
    bool partial = false;         // the walk has not finished this directory
    bool unreadable = false;      // opening or reading it failed
    bool otherFilesystem = false; // on another device than the root, or an automount point; never entered
    // Something below it was not counted (stopped, not entered or unreadable): a lower bound. A folder on another
    // filesystem is never entered, so its record is final as made with this set and nothing counted.
    bool truncated = false;
    bool foldersOmitted = false; // at the detail depth with folders inside, which have no records
    bool hidden = false;         // the name starts with a dot
    bool operator==(const MeasureFlags&) const = default;
  };

  struct MeasureFile {
    std::string name;                 // raw bytes
    std::uint64_t apparentBytes = 0;  // 0 for a later name of a hard-linked file
    std::uint64_t allocatedBytes = 0; // 0 for a later name of a hard-linked file
    std::int64_t modified = 0;
    bool hidden = false;
    bool symlink = false;
    bool repeatedLink = false; // a later name of a hard-linked file: no bytes, not counted as a file
  };

  // Entries folded into one piece: files beyond the file cap, or directories beyond the child cap.
  struct MeasureRemainder {
    std::uint64_t count = 0;
    MeasureTotals totals; // items sums the direct entries of folded directories
  };

  struct MeasureRecord {
    std::string name; // raw bytes; the root's base name, "/" for the filesystem root
    std::uint32_t parent = kNoMeasureRecord;
    std::uint32_t firstChild = 0; // child records are contiguous
    std::uint32_t childCount = 0;
    std::uint32_t depth = 0;
    DirectoryStamp stamp; // zero when the directory was never opened
    MeasureTotals totals;
    MeasureFlags flags;
    // Largest first, then by name; at most the file cap. Null until the directory is listed, then shared by every
    // copy of the table.
    std::shared_ptr<const std::vector<MeasureFile>> files;
    MeasureRemainder otherFiles;
    MeasureRemainder otherDirectories;
  };

  struct MeasureTable {
    std::vector<MeasureRecord> records; // records[0] is the root; parents precede children
    std::uint64_t entries = 0;          // entries examined
    std::uint64_t unreadable = 0;       // directories at any depth that could not be read
    std::uint64_t otherFilesystems = 0; // directories at any depth that were not entered for their device
    bool partial = false;               // handed out while the walk runs
    bool truncated = false;             // the budget, the deadline or the depth limit stopped the walk
    bool cancelled = false;
    bool hardLinksOverflow = false; // more hard-linked files than the bound; later ones count at every name
    std::optional<FileError> error; // the root could not be measured; records is empty
  };

  struct MeasureOptions {
    std::uint32_t detailDepth = 3; // records for the root and this many levels below it
    std::uint32_t fileCap = 24;    // largest files kept per record
    std::uint32_t childCap = 512;  // largest child records kept per record
    std::uint64_t entryBudget = 2'000'000;
    std::uint32_t maxDepth = 128; // deeper directories are counted as items but not entered
    std::size_t hardLinkBound = 100'000;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    std::chrono::milliseconds progressInterval{100};
    CancelToken cancel;
  };

  // Receives a partial table on the walking thread, at most once per progress interval.
  using MeasureProgress = std::function<void(MeasureTable)>;

  // Blocking; for a worker. Never follows a symlink below the root, never leaves the root's device, and counts
  // each directory once by device and inode. Remote and FUSE filesystems give an Unsupported error.
  MeasureTable
  measureSubtree(const Location& root, const MeasureOptions& options, const MeasureProgress& progress = {});

  // Blocking stat of a local directory, following a symlink as the walk's root does; nullopt when it fails.
  std::optional<DirectoryStamp> directoryStamp(const Location& folder);
} // namespace luciole
