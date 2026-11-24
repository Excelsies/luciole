#include "fs/subtree_measure_walk.h"
#include "temp_folder.h"
#include "test_check.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::MeasureOptions;
  using luciole::MeasureRecord;
  using luciole::MeasureTable;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-folded") {}
  };

  void writeFile(const fs::path& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  MeasureTable measure(const fs::path& root, const MeasureOptions& options = {}) {
    return luciole::measureSubtree(luciole::Location::fromPath(root.native()), options);
  }

  const MeasureRecord* child(const MeasureTable& table, std::uint32_t parent, const std::string& name) {
    const auto& record = table.records[parent];
    for (std::uint32_t i = 0; i < record.childCount; ++i) {
      if (table.records[record.firstChild + i].name == name) {
        return &table.records[record.firstChild + i];
      }
    }
    return nullptr;
  }

  // Six folders d0 to d5 with a nested/ of 10 * (i + 1) bytes and a file of 1; d5 also holds six folders e0 to e5 of
  // 100 + j bytes. With two folders kept per record, the others are folded and named in the compact list.
  void foldedChildren(const fs::path& base) {
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
    MeasureOptions options;
    options.childCap = 4;
    const auto table = measure(root, options);
    const auto& top = table.records[0];
    // The two smallest folders are folded: d0 and d1 hold 10 + 1 and 20 + 1 bytes.
    TEST_CHECK(top.folded != nullptr && top.folded->entries.size() == 2 && !top.folded->capped);
    const auto* d0 = top.folded->find("d0");
    const auto* d1 = top.folded->find("d1");
    TEST_CHECK(d0 != nullptr && d0->apparentBytes == 11 && d0->flags == 0);
    TEST_CHECK(d1 != nullptr && d1->apparentBytes == 21 && top.folded->find("d2") == nullptr);
    TEST_CHECK(top.otherDirectories.totals.apparentBytes == d0->apparentBytes + d1->apparentBytes);
    const auto* d5 = child(table, 0, "d5");
    TEST_CHECK(d5 != nullptr && d5->folded != nullptr && d5->folded->entries.size() == 3);
    TEST_CHECK(d5->folded->find("nested")->apparentBytes == 60 && d5->folded->find("e0")->apparentBytes == 100);
    TEST_CHECK(d5->folded->find("e1")->apparentBytes == 101);
    TEST_CHECK(d5->otherDirectories.totals.apparentBytes == 60 + 100 + 101);
    TEST_CHECK(child(table, 0, "d2")->folded == nullptr); // a record that folded nothing holds nothing
  }

  // Eight folders of 10 to 80 bytes, two kept as records: six folded. At the cap of the compact list all six are
  // named; one past it the smallest is left out and the list says it is capped, while the remainder still counts all.
  void foldedCap(const fs::path& base) {
    const auto root = base / "foldedcap";
    for (int i = 0; i < 8; ++i) {
      fs::create_directories(root / std::format("d{}", i));
      writeFile(root / std::format("d{}", i) / "f", 10 * static_cast<std::size_t>(i + 1));
    }
    MeasureOptions options;
    options.childCap = 2;
    options.foldedCap = 6;
    auto table = measure(root, options);
    auto& top = table.records[0];
    TEST_CHECK(top.folded != nullptr && top.folded->entries.size() == 6 && !top.folded->capped);
    for (int i = 0; i < 6; ++i) {
      TEST_CHECK(top.folded->find(std::format("d{}", i))->apparentBytes == 10 * static_cast<std::uint64_t>(i + 1));
    }
    options.foldedCap = 5;
    table = measure(root, options);
    const auto& capped = table.records[0];
    TEST_CHECK(capped.folded != nullptr && capped.folded->entries.size() == 5 && capped.folded->capped);
    TEST_CHECK(capped.folded->find("d0") == nullptr && capped.folded->find("d1") != nullptr);
    TEST_CHECK(capped.otherDirectories.count == 6 && capped.otherDirectories.totals.apparentBytes == 210);
    // The list is sorted by name, and the names are raw bytes.
    for (std::size_t i = 1; i < capped.folded->entries.size(); ++i) {
      TEST_CHECK(capped.folded->nameOf(i - 1) < capped.folded->nameOf(i));
    }
    // A table keeps no spare room for the records that were folded away.
    TEST_CHECK(table.records.capacity() == table.records.size());
  }

  // Four folders of five files of 10 bytes. A child the walk lists and does not enter is marked never entered (and
  // reads as a lower bound of nothing); one it enters and cuts keeps what it counted and is not marked.
  void neverEntered(const fs::path& base) {
    const auto root = base / "never";
    for (int folder = 0; folder < 4; ++folder) {
      fs::create_directories(root / std::format("d{}", folder));
      for (int file = 0; file < 5; ++file) {
        writeFile(root / std::format("d{}", folder) / std::format("f{}", file), 10);
      }
    }
    const auto children = [](const MeasureTable& table) {
      std::vector<const MeasureRecord*> found;
      const auto& top = table.records[0];
      for (std::uint32_t i = 0; i < top.childCount; ++i) {
        found.push_back(&table.records[top.firstChild + i]);
      }
      return found;
    };
    MeasureOptions options;
    options.entryBudget = 4 + 5; // the four folders, then one whole folder
    const auto whole = measure(root, options);
    int complete = 0, unreached = 0;
    for (const auto* entry : children(whole)) {
      if (entry->flags.notEntered) {
        ++unreached;
        TEST_CHECK(entry->flags.truncated && !entry->flags.partial && entry->totals.apparentBytes == 0);
      } else {
        ++complete;
        TEST_CHECK(!entry->flags.truncated && entry->totals.apparentBytes == 50);
      }
    }
    TEST_CHECK(complete == 1 && unreached == 3);
    // Folded children carry the same marks: two records are kept, the other two are entries.
    options.childCap = 2;
    const auto folded = measure(root, options);
    TEST_CHECK(folded.records[0].folded != nullptr && folded.records[0].folded->entries.size() == 2);
    const auto& list = *folded.records[0].folded;
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
      TEST_CHECK(list.entries[i].flags == (luciole::FoldedChildren::kTruncated | luciole::FoldedChildren::kNotEntered));
      TEST_CHECK(list.entries[i].apparentBytes == 0);
    }
    options.childCap = 512;
    options.entryBudget = 4 + 5 + 2; // one whole folder, one cut after two files, two never entered
    const auto cut = measure(root, options);
    int finished = 0, entered = 0;
    unreached = 0;
    for (const auto* entry : children(cut)) {
      if (entry->flags.notEntered) {
        ++unreached;
      } else if (entry->flags.truncated) {
        ++entered;
        TEST_CHECK(entry->totals.apparentBytes == 20);
      } else {
        ++finished;
        TEST_CHECK(entry->totals.apparentBytes == 50);
      }
    }
    TEST_CHECK(finished == 1 && entered == 1 && unreached == 2);
    options.entryBudget = 2'000'000;
    for (const auto* entry : children(measure(root, options))) {
      TEST_CHECK(!entry->flags.notEntered && !entry->flags.truncated);
    }
  }
} // namespace

int main() {
  const Scratch scratch;
  foldedChildren(scratch.path());
  foldedCap(scratch.path());
  neverEntered(scratch.path());
  return 0;
}
