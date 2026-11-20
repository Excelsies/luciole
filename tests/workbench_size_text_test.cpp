#include "browser/entry_details.h"
#include "test_check.h"
#include "workbench/format.h"

#include <array>
#include <cstdint>
#include <print>
#include <string>

// Workbench sizes and the list's Size column write the same text for the same byte count.
namespace {
  using luciole::DetailsColumn;
  using luciole::DirectoryEntry;
  using luciole::FileType;

  constexpr std::uint64_t kKibi = 1024;

  std::string listSize(std::uint64_t bytes) {
    DirectoryEntry entry;
    entry.type = FileType::Regular;
    entry.size = bytes;
    return luciole::entryColumnText(entry, DetailsColumn::Size);
  }

  bool sameText(std::uint64_t bytes) {
    const auto list = listSize(bytes);
    const auto parts = luciole::workbench::sizeText(static_cast<double>(bytes));
    if (list != parts) {
      std::println(stderr, R"({} bytes: list "{}", workbench "{}")", bytes, list, parts);
    }
    return list == parts;
  }

  // Unit edges, rounding edges of one decimal, and sizes the views show often.
  void testTable() {
    constexpr std::array<std::uint64_t, 34> kTable{
        0,
        1,
        2,
        999,
        1023,
        1024,
        1025,
        1075,
        1076,
        1126,
        1127,
        1280,
        1536,
        4000,
        10188,
        10189,
        10239,
        10240,
        102349,
        999 * kKibi,
        1048063,
        1048064,
        1048524,
        1048575,
        1048576,
        1572864,
        1000000000,
        1073741823,
        1073741824,
        5088599865,
        kKibi * kKibi * kKibi * kKibi,
        kKibi * kKibi * kKibi * kKibi * kKibi,
        kKibi * kKibi * kKibi * kKibi * kKibi * kKibi,
        std::uint64_t{1} << 53U,
    };
    for (const auto bytes : kTable) {
      TEST_CHECK(sameText(bytes));
    }
  }

  // Every count up to 4 KiB, then a fixed pseudo-random sweep over sizes a double holds exactly.
  void testSweep() {
    std::size_t differ = 0;
    for (std::uint64_t bytes = 0; bytes <= 4 * kKibi; ++bytes) {
      differ += sameText(bytes) ? 0 : 1;
    }
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 20000; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const int shift = 11 + (i % 42); // from 2^53 down to 2^11
      differ += sameText(state >> static_cast<unsigned>(shift)) ? 0 : 1;
    }
    TEST_CHECK(differ == 0);
  }
} // namespace

int main() {
  testTable();
  testSweep();
  std::println("workbench size text checks passed");
  return 0;
}
