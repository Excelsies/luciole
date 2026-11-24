#include "fs/measure_amend.h"
#include "fs/subtree_measure_walk.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::MeasureAmendment;
  using luciole::MeasureOptions;
  using luciole::MeasureRecord;
  using luciole::MeasureTable;

  struct Scratch : luciole::test::TempFolder {
    Scratch() : TempFolder("luciole-amend") {}
  };

  void writeFile(const fs::path& path, std::size_t bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
    TEST_CHECK(out.good());
  }

  MeasureTable measure(const fs::path& folder, const MeasureOptions& options = {}) {
    return luciole::measureSubtree(luciole::Location::fromPath(folder.native()), options);
  }

  // Folder `i` of `count` holds i + 3 files of 100 bytes, so its size is 100 * (i + 3); the root holds a 7 byte file.
  fs::path tree(const fs::path& base, const std::string& name, int count) {
    const auto root = base / name;
    writeFile(root / "top", 7);
    for (int i = 0; i < count; ++i) {
      for (int file = 0; file < i + 3; ++file) {
        writeFile(root / std::format("s{:03}", i) / std::format("f{}", file), 100);
      }
    }
    return root;
  }

  std::uint64_t expected(const std::string& name) {
    return 100 * (static_cast<std::uint64_t>(std::stoi(name.substr(1))) + 3);
  }

  // The amendment of every never-entered child of the root, by walking each alone.
  std::vector<MeasureAmendment> amendmentsFor(const fs::path& root, const MeasureTable& table) {
    std::vector<MeasureAmendment> result;
    const auto& top = table.records[0];
    for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
      if (table.records[c].flags.notEntered) {
        const auto& name = table.records[c].name;
        result.push_back(luciole::amendmentOf(name, measure(root / name)));
      }
    }
    if (top.folded) {
      for (std::size_t i = 0; i < top.folded->entries.size(); ++i) {
        if ((top.folded->entries[i].flags & luciole::FoldedChildren::kNotEntered) != 0) {
          const std::string name(top.folded->nameOf(i));
          result.push_back(luciole::amendmentOf(name, measure(root / name)));
        }
      }
    }
    return result;
  }

  int neverEnteredCount(const MeasureTable& table) {
    int count = 0;
    const auto& top = table.records[0];
    for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
      count += table.records[c].flags.notEntered ? 1 : 0;
    }
    return count;
  }

  // The budget ends the walk once the root is listed: one child is entered and cut, the others never entered.
  // Which one is entered depends on the order the filesystem lists them, so every check holds for any of them.
  void namedChildren(const fs::path& base) {
    const auto root = tree(base, "named", 6);
    MeasureOptions options;
    options.entryBudget = 7 + 2; // the root's seven entries and two of the entered child's files
    const auto table = measure(root, options);
    TEST_CHECK(table.records[0].flags.truncated);
    const auto amendments = amendmentsFor(root, table);
    TEST_CHECK(amendments.size() == 5);
    const auto amended = luciole::amendedTable(table, amendments);
    TEST_CHECK(amended != nullptr && amended != nullptr && amended->records.size() == table.records.size());
    std::uint64_t sum = 7;
    int cut = 0;
    for (const auto& record : amended->records | std::views::drop(1)) {
      if (record.flags.truncated) {
        ++cut;
        TEST_CHECK(!record.flags.notEntered);           // the entered child stays as the walk left it
        TEST_CHECK(record.totals.apparentBytes == 200); // the two files it counted
        sum += 200;
        continue;
      }
      TEST_CHECK(!record.flags.notEntered && !record.flags.partial);
      TEST_CHECK(record.totals.apparentBytes == expected(record.name));
      TEST_CHECK(record.totals.files == record.totals.apparentBytes / 100);
      sum += record.totals.apparentBytes;
    }
    TEST_CHECK(cut == 1);
    TEST_CHECK(amended->records[0].totals.apparentBytes == sum);
    TEST_CHECK(amended->records[0].flags.truncated);         // the entered child is still a lower bound
    TEST_CHECK(table.records[0].totals.apparentBytes < sum); // the table that was amended is unchanged
    // Amending again, or with a child that is not never-entered, changes nothing.
    TEST_CHECK(luciole::amendedTable(*amended, amendments) == nullptr);
  }

  // With every child but the entered one amended, a root that was cut only by never-entered children is exact.
  void rootBecomesExact(const fs::path& base) {
    const auto root = tree(base, "exact", 3);
    MeasureOptions options;
    options.entryBudget = 4 + 2; // the root's four entries and two files of the entered child
    auto table = measure(root, options);
    // Make the entered child whole by walking it too: nothing below it was cut when it holds one file.
    const auto amendments = amendmentsFor(root, table);
    const auto amended = luciole::amendedTable(table, amendments);
    TEST_CHECK(amended != nullptr);
    const auto& top = amended->records[0];
    const bool anyCut =
        std::ranges::any_of(std::views::iota(top.firstChild, top.firstChild + top.childCount), [&](std::uint32_t c) {
          return amended->records[c].flags.truncated;
        });
    TEST_CHECK(top.flags.truncated == anyCut);
  }

  // Children folded into the root's remainder are amended in their compact list and in the remainder.
  void foldedChildren(const fs::path& base) {
    const auto root = tree(base, "folded", 9);
    MeasureOptions options;
    options.entryBudget = 10 + 2; // the root's ten entries and two files of the entered child
    options.childCap = 3;
    const auto table = measure(root, options);
    const auto& before = table.records[0];
    TEST_CHECK(before.folded != nullptr && before.otherDirectories.count == 6);
    const auto amendments = amendmentsFor(root, table);
    TEST_CHECK(amendments.size() == 8);
    const auto amended = luciole::amendedTable(table, amendments);
    TEST_CHECK(amended != nullptr);
    const auto& top = amended->records[0];
    std::uint64_t foldedSum = 0;
    std::uint64_t sum = 7;
    for (std::size_t i = 0; i < top.folded->entries.size(); ++i) {
      const auto& entry = top.folded->entries[i];
      const std::string name(top.folded->nameOf(i));
      if ((entry.flags & luciole::FoldedChildren::kTruncated) == 0) {
        TEST_CHECK((entry.flags & luciole::FoldedChildren::kNotEntered) == 0);
        TEST_CHECK(entry.apparentBytes == expected(name));
      }
      foldedSum += entry.apparentBytes;
      sum += entry.apparentBytes;
    }
    TEST_CHECK(top.otherDirectories.totals.apparentBytes == foldedSum);
    for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
      sum += amended->records[c].totals.apparentBytes;
    }
    TEST_CHECK(top.totals.apparentBytes == sum);
    TEST_CHECK(before.folded->entries.size() == top.folded->entries.size()); // the old list is not changed
    TEST_CHECK(before.otherDirectories.totals.apparentBytes < top.otherDirectories.totals.apparentBytes);
  }

  void nothingToAmend(const fs::path& base) {
    const auto root = tree(base, "whole", 3);
    const auto table = measure(root);
    TEST_CHECK(luciole::amendedTable(table, {}) == nullptr);
    const auto stray = luciole::amendmentOf("s000", measure(root / "s000"));
    TEST_CHECK(luciole::amendedTable(table, std::span(&stray, 1)) == nullptr); // entered and whole: not amended
    TEST_CHECK(luciole::amendedTable({}, std::span(&stray, 1)) == nullptr);
    const auto missing = luciole::amendmentOf("gone", measure(base / "nowhere"));
    TEST_CHECK(missing.flags.unreadable && missing.flags.truncated && missing.totals.apparentBytes == 0);
  }

  void link(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::create_hard_link(from, to);
  }

  // Three folders, each with a 100 byte file of its own and a name of one 300 byte file, in the folder itself or one
  // level down. The walk of the root enters none: the budget ends it first.
  fs::path linked(const fs::path& base, const std::string& name, bool nested, int own = 1) {
    const auto root = base / name;
    writeFile(root / "s0" / (nested ? "in/lnk" : "lnk"), 300);
    for (int i = 0; i < 3; ++i) {
      for (int file = 0; file < own; ++file) {
        writeFile(root / std::format("s{}", i) / std::format("u{}", file), 100);
      }
      if (i > 0) {
        link(root / "s0" / (nested ? "in/lnk" : "lnk"), root / std::format("s{}", i) / (nested ? "in/lnk" : "lnk"));
      }
    }
    return root;
  }

  // A file linked into several folders the walk never entered counts once in the root, as one walk counts it, and the
  // cells add up to the root.
  void linksAcrossNeverEnteredFolders(const fs::path& base) {
    for (const bool nested : {false, true}) {
      const auto root = linked(base, nested ? "nested" : "direct", nested);
      MeasureOptions options;
      options.entryBudget = 3; // the root's three folders
      const auto table = measure(root, options);
      TEST_CHECK(neverEnteredCount(table) == 3);
      const auto amended = luciole::amendedTable(table, amendmentsFor(root, table));
      TEST_CHECK(amended != nullptr);
      const auto& top = amended->records[0];
      TEST_CHECK(top.totals.apparentBytes == 3 * 100 + 300 && top.totals.files == 3 + 1);
      std::uint64_t cells = 0;
      std::uint64_t files = 0;
      for (auto c = top.firstChild; c < top.firstChild + top.childCount; ++c) {
        cells += amended->records[c].totals.apparentBytes;
        files += amended->records[c].totals.files;
        // The folder that kept the bytes shows the file at full size, the others as a later name.
        if (!nested) {
          for (const auto& file : *amended->records[c].files) {
            TEST_CHECK(file.name != "lnk" || file.repeatedLink == (file.apparentBytes == 0));
          }
        }
      }
      TEST_CHECK(cells == 600 && files == 4);
      TEST_CHECK(top.flags.truncated == false);
      // The same tree measured in one unbounded walk gives the same root.
      TEST_CHECK(measure(root).records[0].totals.apparentBytes == 600);
    }
  }

  // One folder the walk entered, whole, and others it never entered all name the file.
  void linkToAnEnteredFolder(const fs::path& base) {
    const auto root = linked(base, "entered", false, 2);
    MeasureOptions options;
    options.entryBudget = 3 + 3; // the root's folders and the three files of the folder entered first
    const auto table = measure(root, options);
    TEST_CHECK(neverEnteredCount(table) == 2);
    const auto amended = luciole::amendedTable(table, amendmentsFor(root, table));
    TEST_CHECK(amended != nullptr);
    TEST_CHECK(amended->records[0].totals.apparentBytes == 3 * 200 + 300);
    TEST_CHECK(!amended->records[0].flags.truncated);
  }

  // Past the bound of the walk, a later name counts again, as in one walk with that bound.
  void linksPastTheBound(const fs::path& base) {
    const auto root = linked(base, "bound", false);
    MeasureOptions options;
    options.entryBudget = 3;
    options.hardLinkBound = 0;
    const auto table = measure(root, options);
    const auto amended = luciole::amendedTable(table, amendmentsFor(root, table));
    TEST_CHECK(amended != nullptr && amended->records[0].totals.apparentBytes == 3 * 100 + 3 * 300);
    TEST_CHECK(amended->hardLinksOverflow);
    MeasureOptions whole;
    whole.hardLinkBound = 0;
    TEST_CHECK(measure(root, whole).records[0].totals.apparentBytes == 3 * 100 + 3 * 300);
  }
} // namespace

int main() {
  const Scratch scratch;
  namedChildren(scratch.path());
  rootBecomesExact(scratch.path());
  foldedChildren(scratch.path());
  nothingToAmend(scratch.path());
  linksAcrossNeverEnteredFolders(scratch.path());
  linkToAnEnteredFolder(scratch.path());
  linksPastTheBound(scratch.path());
  return 0;
}
