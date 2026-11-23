#include "fs/subtree_measure_walk.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::MeasureOptions;
  using luciole::MeasureRecord;
  using luciole::MeasureTable;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-measure") {}
  };

  void writeFile(const fs::path& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  void setTime(const fs::path& path, std::int64_t seconds) {
    const timespec times[2] = {{.tv_sec = seconds, .tv_nsec = 0}, {.tv_sec = seconds, .tv_nsec = 0}};
    TEST_CHECK(::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0);
  }

  MeasureTable measure(const fs::path& root, const MeasureOptions& options = {}) {
    return luciole::measureSubtree(luciole::Location::fromPath(root.native()), options);
  }

  const MeasureRecord* child(const MeasureTable& table, std::uint32_t parent, const std::string& name) {
    const auto& record = table.records[parent];
    for (std::uint32_t i = 0; i < record.childCount; ++i) {
      const auto& candidate = table.records[record.firstChild + i];
      if (candidate.name == name) {
        return &candidate;
      }
    }
    return nullptr;
  }

  std::uint32_t indexOf(const MeasureTable& table, const MeasureRecord* record) {
    return static_cast<std::uint32_t>(record - table.records.data());
  }

  // Parents precede children, each child block is contiguous, and every record points back at its parent.
  void checkShape(const MeasureTable& table) {
    TEST_CHECK(!table.records.empty() && table.records[0].parent == luciole::kNoMeasureRecord);
    for (std::uint32_t i = 0; i < table.records.size(); ++i) {
      const auto& record = table.records[i];
      if (i > 0) {
        TEST_CHECK(record.parent < i);
        TEST_CHECK(record.depth == table.records[record.parent].depth + 1);
      }
      for (std::uint32_t c = 0; c < record.childCount; ++c) {
        TEST_CHECK(record.firstChild + c < table.records.size());
        TEST_CHECK(table.records[record.firstChild + c].parent == i);
      }
    }
  }

  // root/a.txt 100, root/.hid 10, root/link -> sub (3), root/sub/b.bin 1000, root/sub/deep/c.dat 50, root/empty.
  fs::path knownTree(const fs::path& base) {
    const auto root = base / "known";
    fs::create_directories(root / "sub" / "deep");
    fs::create_directory(root / "empty");
    writeFile(root / "a.txt", 100);
    writeFile(root / ".hid", 10);
    writeFile(root / "sub" / "b.bin", 1000);
    writeFile(root / "sub" / "deep" / "c.dat", 50);
    fs::create_directory_symlink("sub", root / "link");
    setTime(root / "a.txt", 1000);
    setTime(root / ".hid", 2000);
    setTime(root / "link", 300);
    setTime(root / "sub" / "b.bin", 3000);
    setTime(root / "sub" / "deep" / "c.dat", 5000);
    setTime(root / "sub" / "deep", 200);
    setTime(root / "sub", 200);
    setTime(root / "empty", 400);
    setTime(root, 100);
    return root;
  }

  void knownTotals(const fs::path& base) {
    const auto root = knownTree(base);
    const auto table = measure(root);
    TEST_CHECK(!table.error && !table.partial && !table.truncated && !table.cancelled);
    checkShape(table);
    TEST_CHECK(table.records.size() == 4);
    TEST_CHECK(table.entries == 8);
    const auto& top = table.records[0];
    TEST_CHECK(top.name == "known" && top.depth == 0);
    TEST_CHECK(top.totals.apparentBytes == 100 + 10 + 3 + 1000 + 50);
    TEST_CHECK(top.totals.files == 5);
    TEST_CHECK(top.totals.items == 5);
    TEST_CHECK(top.totals.newest == 5000);
    TEST_CHECK(top.totals.allocatedBytes >= 100 + 10 + 1000 + 50);
    TEST_CHECK(!top.flags.partial && !top.flags.hidden && !top.flags.truncated);
    TEST_CHECK(top.childCount == 2);
    const auto& kept = *top.files;
    TEST_CHECK(kept.size() == 3);
    TEST_CHECK(kept[0].name == "a.txt" && kept[0].apparentBytes == 100 && kept[0].modified == 1000);
    TEST_CHECK(kept[1].name == ".hid" && kept[1].hidden);
    TEST_CHECK(kept[2].name == "link" && kept[2].symlink && kept[2].apparentBytes == 3);
    TEST_CHECK(top.otherFiles.count == 0);

    const auto* sub = child(table, 0, "sub");
    TEST_CHECK(sub != nullptr && sub->depth == 1);
    TEST_CHECK(sub->totals.apparentBytes == 1050 && sub->totals.files == 2 && sub->totals.items == 2);
    TEST_CHECK(sub->totals.newest == 5000);
    TEST_CHECK(sub->files->size() == 1 && sub->files->front().name == "b.bin");
    const auto* deep = child(table, indexOf(table, sub), "deep");
    TEST_CHECK(deep != nullptr && deep->depth == 2);
    TEST_CHECK(deep->totals.apparentBytes == 50 && deep->totals.files == 1 && deep->totals.items == 1);
    const auto* empty = child(table, 0, "empty");
    TEST_CHECK(empty != nullptr && empty->totals.items == 0 && empty->totals.files == 0);
    TEST_CHECK(empty->totals.newest == 400 && empty->totals.apparentBytes == 0);

    struct stat info{};
    TEST_CHECK(::stat(root.c_str(), &info) == 0);
    TEST_CHECK(top.stamp.inode == info.st_ino && top.stamp.device == info.st_dev);
    const auto stamp = luciole::directoryStamp(luciole::Location::fromPath(root.native()));
    TEST_CHECK(stamp && *stamp == top.stamp);
    writeFile(root / "added", 1);
    const auto after = luciole::directoryStamp(luciole::Location::fromPath(root.native()));
    TEST_CHECK(after && after->inode == top.stamp.inode && *after != top.stamp);
    TEST_CHECK(!luciole::directoryStamp(luciole::Location::fromPath((root / "missing").native())));
  }

  // A folder time past 2262 has no 64-bit count of nanoseconds since 1970.
  void farFutureStamp(const fs::path& base) {
    const auto root = base / "future";
    fs::create_directory(root);
    setTime(root, 10'000'000'000);
    struct stat info{};
    TEST_CHECK(::stat(root.c_str(), &info) == 0 && info.st_mtim.tv_sec == 10'000'000'000);
    const auto table = measure(root);
    const auto& stamp = table.records[0].stamp;
    TEST_CHECK(stamp.modifiedSeconds == info.st_mtim.tv_sec && stamp.modifiedNanoseconds == info.st_mtim.tv_nsec);
    TEST_CHECK(stamp.changedSeconds == info.st_ctim.tv_sec && stamp.changedNanoseconds == info.st_ctim.tv_nsec);
    const auto again = luciole::directoryStamp(luciole::Location::fromPath(root.native()));
    TEST_CHECK(again && *again == stamp);
  }

  void sparseAllocation(const fs::path& base) {
    const auto root = base / "sparse";
    fs::create_directory(root);
    const int fd = ::open((root / "hole").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    TEST_CHECK(fd >= 0 && ::ftruncate(fd, 1048576) == 0 && ::close(fd) == 0);
    const auto table = measure(root);
    TEST_CHECK(table.records[0].totals.apparentBytes == 1048576);
    TEST_CHECK(table.records[0].files->size() == 1 && table.records[0].files->front().allocatedBytes < 1048576);
  }

  void symlinksAndLoops(const fs::path& base) {
    const auto root = base / "loops";
    fs::create_directories(root / "inner");
    fs::create_directory_symlink("..", root / "inner" / "up");
    fs::create_directory_symlink(".", root / "self");
    fs::create_symlink("missing", root / "broken");
    const auto table = measure(root);
    TEST_CHECK(!table.error && !table.truncated);
    checkShape(table);
    TEST_CHECK(table.records.size() == 2);
    TEST_CHECK(table.records[0].totals.files == 3);
    TEST_CHECK(table.records[0].totals.apparentBytes == 2 + 1 + 7);
    TEST_CHECK(table.entries == 4);

    // A symlinked root is followed; nothing below it is.
    fs::create_directory_symlink("loops", base / "loops-link");
    const auto linked = measure(base / "loops-link");
    TEST_CHECK(!linked.error && linked.records[0].totals == table.records[0].totals);
    TEST_CHECK(measure(root / "broken").error.has_value());
    TEST_CHECK(measure(base / "nowhere").error.has_value());
  }

  void hardLinks(const fs::path& base) {
    const auto root = base / "hard";
    fs::create_directories(root / "sub");
    writeFile(root / "f", 300);
    fs::create_hard_link(root / "f", root / "g");
    fs::create_hard_link(root / "f", root / "sub" / "h");
    const auto table = measure(root);
    const auto& top = table.records[0];
    TEST_CHECK(top.totals.apparentBytes == 300 && top.totals.files == 1 && top.totals.items == 3);
    TEST_CHECK(!table.hardLinksOverflow);
    const auto* sub = child(table, 0, "sub");
    TEST_CHECK(sub != nullptr && sub->totals.items == 1 && sub->totals.files == 0 && sub->totals.apparentBytes == 0);
    TEST_CHECK(sub->files->size() == 1 && sub->files->front().apparentBytes == 0);

    // Beyond the bound, a hard-linked file counts at every name.
    MeasureOptions options;
    options.hardLinkBound = 0;
    const auto unbounded = measure(root, options);
    TEST_CHECK(unbounded.hardLinksOverflow);
    TEST_CHECK(unbounded.records[0].totals.apparentBytes == 900 && unbounded.records[0].totals.files == 3);
  }

  void unreadableDirectory(const fs::path& base) {
    const auto root = base / "locked-tree";
    fs::create_directories(root / "locked");
    writeFile(root / "locked" / "inside", 40);
    writeFile(root / "open", 5);
    fs::permissions(root / "locked", fs::perms::none);
    const auto table = measure(root);
    const auto lockedRoot = measure(root / "locked");
    fs::permissions(root / "locked", fs::perms::owner_all);
    if (::geteuid() == 0) {
      return; // Permission bits do not stop the superuser.
    }
    TEST_CHECK(!table.error && table.unreadable == 1);
    const auto* locked = child(table, 0, "locked");
    TEST_CHECK(locked != nullptr && locked->flags.unreadable && !locked->flags.partial);
    TEST_CHECK(locked->totals.items == 0 && locked->totals.files == 0);
    TEST_CHECK(table.records[0].totals.apparentBytes == 5 && table.records[0].totals.items == 2);
    TEST_CHECK(lockedRoot.error && lockedRoot.records.empty());

    // A folder that can be listed but not searched: its entries count as items, nothing else is known.
    const auto listOnly = base / "list-only";
    fs::create_directories(listOnly / "names");
    writeFile(listOnly / "names" / "one", 3);
    writeFile(listOnly / "names" / "two", 4);
    fs::permissions(listOnly / "names", fs::perms::owner_read);
    const auto listed = measure(listOnly);
    fs::permissions(listOnly / "names", fs::perms::owner_all);
    const auto* names = child(listed, 0, "names");
    TEST_CHECK(names != nullptr && names->flags.unreadable && listed.unreadable == 1);
    TEST_CHECK(names->totals.items == 2 && names->totals.files == 0 && names->totals.apparentBytes == 0);
  }

  void manyFiles(const fs::path& base) {
    const auto root = base / "many";
    fs::create_directory(root);
    for (int i = 0; i < 20000; ++i) {
      writeFile(root / std::format("f{:05}", i), i < 30 ? 1000 + static_cast<std::size_t>(i) : 1);
    }
    const auto table = measure(root);
    const auto& top = table.records[0];
    TEST_CHECK(table.entries == 20000 && table.records.size() == 1);
    TEST_CHECK(top.totals.items == 20000 && top.totals.files == 20000);
    TEST_CHECK(top.totals.apparentBytes == 30000 + 435 + 19970);
    const auto& kept = *top.files;
    TEST_CHECK(kept.size() == 24);
    TEST_CHECK(kept[0].name == "f00029" && kept[0].apparentBytes == 1029);
    TEST_CHECK(kept[23].name == "f00006" && kept[23].apparentBytes == 1006);
    TEST_CHECK(top.otherFiles.count == 19976 && top.otherFiles.totals.files == 19976);
    TEST_CHECK(top.otherFiles.totals.apparentBytes == 6015 + 19970);

    MeasureOptions none;
    none.fileCap = 0;
    const auto folded = measure(root, none);
    TEST_CHECK(folded.records[0].files->empty() && folded.records[0].otherFiles.count == 20000);
    TEST_CHECK(folded.records[0].otherFiles.totals.apparentBytes == top.totals.apparentBytes);
  }

  void deepTree(const fs::path& base) {
    const auto root = base / "deep";
    auto path = root;
    for (int level = 0; level < 40; ++level) {
      path /= std::format("l{}", level);
    }
    fs::create_directories(path);
    writeFile(path / "bottom", 7);
    constexpr std::int64_t kFuture = 4'000'000'000;
    setTime(path / "bottom", kFuture);
    const auto table = measure(root);
    checkShape(table);
    TEST_CHECK(!table.truncated && table.records.size() == 4);
    TEST_CHECK(table.records[0].totals.files == 1 && table.records[0].totals.apparentBytes == 7);
    TEST_CHECK(table.records[0].totals.newest == kFuture);
    TEST_CHECK(table.records[3].depth == 3 && table.records[3].name == "l2");
    TEST_CHECK(table.records[3].totals.files == 1 && table.records[3].childCount == 0);
    TEST_CHECK(table.records[3].flags.foldersOmitted && !table.records[2].flags.foldersOmitted);

    MeasureOptions shallow;
    shallow.maxDepth = 10;
    const auto limited = measure(root, shallow);
    TEST_CHECK(limited.truncated && limited.records[0].flags.truncated && limited.records[3].flags.truncated);
    TEST_CHECK(limited.records[0].totals.files == 0 && limited.records[0].totals.apparentBytes == 0);
  }

  void detailDepth(const fs::path& base) {
    fs::create_directory(base / "detail");
    const auto root = knownTree(base / "detail");
    const auto full = measure(root);
    MeasureOptions options;
    options.detailDepth = 1;
    const auto one = measure(root, options);
    checkShape(one);
    TEST_CHECK(one.records.size() == 3 && one.records[0].childCount == 2);
    TEST_CHECK(one.records[0].totals == full.records[0].totals);
    TEST_CHECK(child(one, 0, "sub")->totals == child(full, 0, "sub")->totals);
    TEST_CHECK(child(one, 0, "sub")->childCount == 0 && child(one, 0, "sub")->files->size() == 1);
    TEST_CHECK(child(one, 0, "sub")->flags.foldersOmitted && !child(one, 0, "empty")->flags.foldersOmitted);
    TEST_CHECK(!one.records[0].flags.foldersOmitted);
    TEST_CHECK(std::ranges::none_of(full.records, [](const MeasureRecord& r) { return r.flags.foldersOmitted; }));
    options.detailDepth = 0;
    const auto zero = measure(root, options);
    TEST_CHECK(zero.records.size() == 1 && zero.records[0].childCount == 0 && zero.records[0].flags.foldersOmitted);
    TEST_CHECK(zero.records[0].totals == full.records[0].totals && zero.records[0].files->size() == 3);
  }

  // d0..d5 each hold nested/data of 10 * (i + 1) bytes and own (1 byte); d5 also holds e0..e5 of 100 + j bytes.
  void caps(const fs::path& base) {
    const auto root = base / "capped";
    for (int i = 0; i < 6; ++i) {
      const auto folder = root / std::format("d{}", i);
      fs::create_directories(folder / "nested");
      writeFile(folder / "nested" / "data", 10 * static_cast<std::size_t>(i + 1));
      writeFile(folder / "own", 1);
    }
    for (int j = 0; j < 6; ++j) {
      fs::create_directory(root / "d5" / std::format("e{}", j));
      writeFile(root / "d5" / std::format("e{}", j) / "x", 100 + static_cast<std::size_t>(j));
    }
    writeFile(root / "a", 3);
    writeFile(root / "b", 2);
    writeFile(root / "c", 1);
    MeasureOptions options;
    options.childCap = 4;
    options.fileCap = 2;
    const auto table = measure(root, options);
    checkShape(table);
    const auto& top = table.records[0];
    TEST_CHECK(top.childCount == 4 && table.records.size() == 1 + 4 + 3 + 4);
    TEST_CHECK(child(table, 0, "d0") == nullptr && child(table, 0, "d1") == nullptr);
    for (int i = 2; i < 5; ++i) {
      const auto* folder = child(table, 0, std::format("d{}", i));
      TEST_CHECK(folder != nullptr && folder->totals.apparentBytes == 10 * static_cast<std::uint64_t>(i + 1) + 1);
      const auto* nested = child(table, indexOf(table, folder), "nested");
      TEST_CHECK(nested != nullptr && nested->totals.apparentBytes == 10 * static_cast<std::uint64_t>(i + 1));
    }
    const auto* d5 = child(table, 0, "d5");
    TEST_CHECK(d5 != nullptr && d5->childCount == 4 && d5->totals.apparentBytes == 60 + 1 + 615);
    TEST_CHECK(
        child(table, indexOf(table, d5), "nested") == nullptr && child(table, indexOf(table, d5), "e0") == nullptr
    );
    TEST_CHECK(child(table, indexOf(table, d5), "e5")->totals.apparentBytes == 105);
    TEST_CHECK(d5->otherDirectories.count == 3 && d5->otherDirectories.totals.apparentBytes == 60 + 100 + 101);
    TEST_CHECK(top.otherDirectories.count == 2 && top.otherDirectories.totals.apparentBytes == 10 + 1 + 20 + 1);
    TEST_CHECK(top.otherDirectories.totals.files == 4 && top.otherDirectories.totals.items == 4);
    TEST_CHECK(top.totals.apparentBytes == 210 + 6 + 615 + 6);
    TEST_CHECK(top.files->size() == 2 && top.files->at(0).name == "a" && top.files->at(1).name == "b");
    TEST_CHECK(top.otherFiles.count == 1 && top.otherFiles.totals.apparentBytes == 1);
  }

  void budget(const fs::path& base) {
    fs::create_directory(base / "budget");
    const auto root = knownTree(base / "budget");
    MeasureOptions options;
    options.entryBudget = 3;
    const auto table = measure(root, options);
    checkShape(table);
    TEST_CHECK(table.truncated && !table.cancelled && table.entries == 3);
    TEST_CHECK(table.records[0].flags.truncated && !table.records[0].flags.partial);
    TEST_CHECK(table.records[0].totals.items == 3);
    // The tree has 8 entries. A budget of exactly 8 may cut before the empty folder, which is not read to find out it
    // is empty, or not, by the order the filesystem lists its folders in; one more always finishes.
    options.entryBudget = 8;
    TEST_CHECK(measure(root, options).entries == 8);
    options.entryBudget = 9;
    const auto exact = measure(root, options);
    TEST_CHECK(!exact.truncated && exact.records[0].totals.files == 5);
    options.entryBudget = 2'000'000;
    options.deadline = std::chrono::steady_clock::now();
    const auto late = measure(root, options);
    TEST_CHECK(late.truncated && late.records[0].flags.truncated);
  }

  // 40 folders, each with inner/ holding 100 files of 10 bytes.
  fs::path wideTree(const fs::path& base) {
    const auto root = base / "wide";
    for (int d = 0; d < 40; ++d) {
      const auto folder = root / std::format("d{:02}", d) / "inner";
      fs::create_directories(folder);
      for (int f = 0; f < 100; ++f) {
        writeFile(folder / std::format("f{}", f), 10);
      }
    }
    return root;
  }

  void cancellation(const fs::path& root) {
    MeasureOptions options;
    options.progressInterval = std::chrono::milliseconds(0);
    int calls = 0;
    const auto table =
        luciole::measureSubtree(luciole::Location::fromPath(root.native()), options, [&](const MeasureTable&) {
          ++calls;
          options.cancel.cancel();
        });
    TEST_CHECK(calls == 1 && table.cancelled && !table.truncated);
    TEST_CHECK(table.records[0].flags.partial && table.entries < 1000);
  }

  void growingPartials(const fs::path& root) {
    MeasureOptions options;
    options.progressInterval = std::chrono::milliseconds(0);
    struct Seen {
      luciole::MeasureTotals totals;
      std::size_t records = 0;
      std::uint64_t entries = 0;
    };
    std::vector<Seen> seen;
    std::vector<const void*> lists; // each finished record's file list, shared by every later copy
    const auto table =
        luciole::measureSubtree(luciole::Location::fromPath(root.native()), options, [&](const MeasureTable& partial) {
          TEST_CHECK(partial.partial && partial.records[0].flags.partial);
          checkShape(partial);
          lists.resize(partial.records.size(), nullptr);
          for (std::size_t i = 0; i < partial.records.size(); ++i) {
            const void* list = partial.records[i].flags.partial ? nullptr : partial.records[i].files.get();
            TEST_CHECK(lists[i] == nullptr || lists[i] == list);
            lists[i] = list;
          }
          std::uint64_t below = 0;
          for (std::uint32_t c = 0; c < partial.records[0].childCount; ++c) {
            below += partial.records[partial.records[0].firstChild + c].totals.apparentBytes;
          }
          TEST_CHECK(below == partial.records[0].totals.apparentBytes);
          seen.push_back({partial.records[0].totals, partial.records.size(), partial.entries});
        });
    TEST_CHECK(seen.size() >= 40);
    for (std::size_t i = 1; i < seen.size(); ++i) {
      TEST_CHECK(seen[i].totals.apparentBytes >= seen[i - 1].totals.apparentBytes);
      TEST_CHECK(seen[i].totals.files >= seen[i - 1].totals.files);
      TEST_CHECK(seen[i].totals.newest >= seen[i - 1].totals.newest);
      TEST_CHECK(seen[i].records >= seen[i - 1].records && seen[i].entries >= seen[i - 1].entries);
    }
    TEST_CHECK(!table.partial && !table.records[0].flags.partial);
    TEST_CHECK(table.records[0].totals.apparentBytes == 40000 && table.records[0].totals.files == 4000);
    TEST_CHECK(seen.back().totals.apparentBytes <= 40000 && table.records.size() == 1 + 40 + 40);
    TEST_CHECK(lists.size() == table.records.size() && lists[1] != nullptr);
    for (std::size_t i = 0; i < lists.size(); ++i) {
      TEST_CHECK(lists[i] == nullptr || lists[i] == table.records[i].files.get());
    }
  }

  // Only the listing of "/" is read: the budget equals its entry count, so nothing below it is opened.
  void otherFilesystem() {
    const auto count =
        static_cast<std::uint64_t>(std::ranges::distance(fs::directory_iterator("/"), fs::directory_iterator()));
    MeasureOptions options;
    options.detailDepth = 1;
    options.entryBudget = count;
    const auto table = luciole::measureSubtree(luciole::Location::fromPath("/"), options);
    TEST_CHECK(!table.error && table.entries == count && table.records[0].name == "/");
    const auto* proc = child(table, 0, "proc");
    TEST_CHECK(proc != nullptr && proc->flags.otherFilesystem && !proc->flags.partial);
    TEST_CHECK(table.otherFilesystems >= 1);
    TEST_CHECK(std::ranges::all_of(table.records, [](const MeasureRecord& record) { return record.depth <= 1; }));
  }
} // namespace

int main() {
  const Scratch scratch;
  knownTotals(scratch.path());
  farFutureStamp(scratch.path());
  sparseAllocation(scratch.path());
  symlinksAndLoops(scratch.path());
  hardLinks(scratch.path());
  unreadableDirectory(scratch.path());
  manyFiles(scratch.path());
  deepTree(scratch.path());
  detailDepth(scratch.path());
  caps(scratch.path());
  budget(scratch.path());
  const auto wide = wideTree(scratch.path());
  cancellation(wide);
  growingPartials(wide);
  otherFilesystem();
  return 0;
}
