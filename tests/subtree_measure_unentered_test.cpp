#include "fs/subtree_measure_walk.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// A directory the walk did not enter, or could not read, is a lower bound with nothing counted, and every directory
// above it is a lower bound too.
namespace {
  namespace fs = std::filesystem;
  using luciole::MeasureRecord;
  using luciole::MeasureTable;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-unentered") {}
  };

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  MeasureTable measure(const fs::path& root, const luciole::MeasureOptions& options = {}) {
    return luciole::measureSubtree(luciole::Location::fromPath(root.native()), options);
  }

  const MeasureRecord* find(const MeasureTable& table, const MeasureRecord& parent, const std::string& name) {
    for (std::uint32_t i = 0; i < parent.childCount; ++i) {
      if (table.records[parent.firstChild + i].name == name) {
        return &table.records[parent.firstChild + i];
      }
    }
    return nullptr;
  }

  // root/{a/{locked/{inside 40}, ok 10}, fine/{f 3}, top 5}
  void readableTree(const fs::path& base) {
    const auto root = base / "readable";
    writeFile(root / "a" / "locked" / "inside", 40);
    writeFile(root / "a" / "ok", 10);
    writeFile(root / "fine" / "f", 3);
    writeFile(root / "top", 5);
    const auto table = measure(root);
    TEST_CHECK(!table.error && table.unreadable == 0);
    TEST_CHECK(std::ranges::none_of(table.records, [](const MeasureRecord& r) { return r.flags.truncated; }));
    TEST_CHECK(table.records[0].totals.apparentBytes == 58);
  }

  void unreadableChild(const fs::path& base) {
    const auto root = base / "readable";
    fs::permissions(root / "a" / "locked", fs::perms::none);
    const auto table = measure(root);
    fs::permissions(root / "a" / "locked", fs::perms::owner_all);
    if (::geteuid() == 0) {
      std::println("skipped: unreadable child (permission bits do not stop the superuser)");
      return;
    }
    const auto* a = find(table, table.records[0], "a");
    TEST_CHECK(a != nullptr);
    const auto* locked = find(table, *a, "locked");
    const auto* fine = find(table, table.records[0], "fine");
    TEST_CHECK(locked != nullptr && fine != nullptr);
    TEST_CHECK(locked->flags.unreadable && locked->flags.truncated && !locked->flags.partial);
    TEST_CHECK(locked->totals.apparentBytes == 0 && locked->totals.files == 0);
    TEST_CHECK(a->flags.truncated && table.records[0].flags.truncated);
    TEST_CHECK(!fine->flags.truncated);
    TEST_CHECK(table.records[0].totals.apparentBytes == 18); // 10 + 3 + 5, the locked file is not counted
  }

  // A folder that can be listed but not searched: its entries cannot be examined, so it and its parents are cut short.
  void listOnlyChild(const fs::path& base) {
    const auto root = base / "list-only";
    writeFile(root / "names" / "one", 3);
    writeFile(root / "names" / "two", 4);
    fs::permissions(root / "names", fs::perms::owner_read);
    const auto table = measure(root);
    fs::permissions(root / "names", fs::perms::owner_all);
    if (::geteuid() == 0) {
      std::println("skipped: list-only child (permission bits do not stop the superuser)");
      return;
    }
    const auto* names = find(table, table.records[0], "names");
    TEST_CHECK(names != nullptr && names->flags.unreadable && names->flags.truncated);
    TEST_CHECK(table.records[0].flags.truncated);
  }

  bool otherDevice(const fs::path& child, const fs::path& parent) {
    struct stat inner{};
    struct stat outer{};
    return ::stat(child.c_str(), &inner) == 0 && ::stat(parent.c_str(), &outer) == 0 && inner.st_dev != outer.st_dev;
  }

  // A mount point under /dev (small, never listed beyond its names) is not entered, and the folder above it is a lower
  // bound. The walk has no budget, so nothing else can cut it short.
  void otherFilesystemChild() {
    const fs::path parent = "/dev";
    for (const auto* name : {"shm", "pts"}) {
      if (!otherDevice(parent / name, parent)) {
        continue;
      }
      const auto table = measure(parent);
      const auto* mount = find(table, table.records[0], name);
      if (table.error || table.unreadable != 0) {
        std::println("skipped: other-filesystem child ({} could not be read whole)", parent.string());
        return;
      }
      TEST_CHECK(mount != nullptr && mount->flags.otherFilesystem);
      TEST_CHECK(mount->flags.truncated && !mount->flags.partial && mount->totals.apparentBytes == 0);
      TEST_CHECK(table.records[0].flags.truncated && !table.truncated);
      return;
    }
    std::println("skipped: other-filesystem child (/dev/shm and /dev/pts are not separate filesystems)");
  }

  // A folder that another walk skipped for its device reads exact when it is the walk's own root.
  void otherFilesystemRoot() {
    struct stat shm{};
    struct stat dev{};
    if (::stat("/dev/shm", &shm) != 0 || ::stat("/dev", &dev) != 0 || shm.st_dev == dev.st_dev) {
      std::println("skipped: other-filesystem root (/dev/shm is absent or not a separate filesystem)");
      return;
    }
    const luciole::test::TempFolder folder("luciole-unentered-shm", "/dev/shm");
    writeFile(folder.path() / "x" / "file", 9);
    const auto table = measure(folder.path());
    TEST_CHECK(!table.error && !table.records[0].flags.truncated && table.records[0].totals.apparentBytes == 9);
  }
} // namespace

int main() {
  const Scratch scratch;
  readableTree(scratch.path());
  unreadableChild(scratch.path());
  listOnlyChild(scratch.path());
  otherFilesystemChild();
  otherFilesystemRoot();
  return 0;
}
