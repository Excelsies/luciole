#include "formats/hex_dump.h"
#include "test_check.h"

#include <cstddef>
#include <limits>
#include <print>
#include <string>
#include <string_view>

namespace {
  using luciole::formats::BlockKind;
  using luciole::formats::hexDump;
  using luciole::formats::joinedText;
  using luciole::formats::kHexDumpBytes;
  using luciole::formats::StyledDocument;
  using luciole::formats::StyledLine;
  using luciole::formats::StyleRole;
  using namespace std::string_view_literals;

  const StyledLine& line(const StyledDocument& document, std::size_t index) {
    TEST_CHECK(document.blocks.size() == 1);
    TEST_CHECK(index < document.blocks[0].lines.size());
    return document.blocks[0].lines[index];
  }

  // The role of the run holding the byte at `column` of the line's text.
  StyleRole roleAt(const StyledLine& styled, std::size_t column) {
    std::size_t start = 0;
    for (const auto& run : styled.runs) {
      if (column < start + run.text.size()) {
        return run.role;
      }
      start += run.text.size();
    }
    TEST_CHECK(false);
    return StyleRole::Plain;
  }

  void testRows() {
    const auto document = hexDump("Hello, world!\n\x00\x01\xFF\x7F"sv);
    TEST_CHECK(document.blocks.size() == 1 && document.blocks[0].kind == BlockKind::Code);
    TEST_CHECK(document.blocks[0].lines.size() == 2);
    TEST_CHECK(
        joinedText(line(document, 0))
        == "00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a 00 01  |Hello, world!...|"
    );
    TEST_CHECK(joinedText(line(document, 1)) == "00000010  ff 7f" + std::string(45, ' ') + "|..|");
    TEST_CHECK(!document.truncated() && !document.problem);
  }

  void testRoles() {
    const auto document = hexDump("A\x00\x01"sv);
    const auto& row = line(document, 0);
    TEST_CHECK(joinedText(row) == "00000000  41 00 01" + std::string(42, ' ') + "|A..|");
    TEST_CHECK(roleAt(row, 0) == StyleRole::Comment);      // offset
    TEST_CHECK(roleAt(row, 10) == StyleRole::Number);      // 41
    TEST_CHECK(roleAt(row, 13) == StyleRole::Punctuation); // 00 is dimmed
    TEST_CHECK(roleAt(row, 16) == StyleRole::Number);      // 01
    TEST_CHECK(roleAt(row, 60) == StyleRole::Punctuation); // the bar
    TEST_CHECK(roleAt(row, 61) == StyleRole::Plain);       // A
    TEST_CHECK(roleAt(row, 62) == StyleRole::Punctuation); // a dot for an unprintable byte
    for (const auto& run : row.runs) {
      for (const char c : run.text) {
        TEST_CHECK(c >= 0x20 && c < 0x7F);
      }
    }
  }

  void testEmpty() {
    const auto document = hexDump({});
    TEST_CHECK(document.blocks.size() == 1 && document.blocks[0].lines.empty());
    TEST_CHECK(!document.truncated());
  }

  void testCap() {
    const std::string exact(kHexDumpBytes, 'x');
    const auto whole = hexDump(exact);
    TEST_CHECK(whole.blocks[0].lines.size() == 4096 && !whole.bytesCapped);
    const auto cut = hexDump(exact + "more");
    TEST_CHECK(cut.blocks[0].lines.size() == 4096 && cut.bytesCapped && cut.truncated());
    TEST_CHECK(joinedText(cut.blocks[0].lines.back()).starts_with("0000fff0  78 78"));
  }

  // Narrower rows keep the groups of eight; a count between the choices takes the widest choice below it, and one
  // below the narrowest takes it.
  void testNarrowRows() {
    const auto bytes = "Hello, world!\n\x00\x01\xFF\x7F"sv;
    const auto twelve = hexDump(bytes, 12);
    TEST_CHECK(twelve.blocks[0].lines.size() == 2);
    TEST_CHECK(joinedText(line(twelve, 0)) == "00000000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64  |Hello, world|");
    TEST_CHECK(joinedText(line(twelve, 1)) == "0000000c  21 0a 00 01 ff 7f" + std::string(21, ' ') + "|!.....|");
    const auto eight = hexDump(bytes, 8);
    TEST_CHECK(eight.blocks[0].lines.size() == 3);
    TEST_CHECK(joinedText(line(eight, 0)) == "00000000  48 65 6c 6c 6f 2c 20 77  |Hello, w|");
    TEST_CHECK(joinedText(line(eight, 2)) == "00000010  ff 7f" + std::string(20, ' ') + "|..|");
    TEST_CHECK(joinedText(line(hexDump(bytes, 15), 0)) == joinedText(line(twelve, 0)));
    const auto four = hexDump(bytes, 4);
    TEST_CHECK(four.blocks[0].lines.size() == 5);
    TEST_CHECK(joinedText(line(four, 0)) == "00000000  48 65 6c 6c  |Hell|");
    TEST_CHECK(joinedText(line(four, 4)) == "00000010  ff 7f" + std::string(8, ' ') + "|..|");
    TEST_CHECK(joinedText(line(hexDump(bytes, 7), 0)) == joinedText(line(four, 0)));
    TEST_CHECK(joinedText(line(hexDump(bytes, 0), 0)) == joinedText(line(four, 0)));
    TEST_CHECK(joinedText(line(hexDump(bytes, 1000), 0)) == joinedText(line(hexDump(bytes), 0)));
  }

  // A row's width in characters, and the widest choice that fits a number of columns.
  void testRowFitting() {
    using luciole::formats::hexDumpRowsFitting;
    using luciole::formats::hexDumpRowWidth;
    TEST_CHECK(
        hexDumpRowWidth(16) == 78 && hexDumpRowWidth(12) == 62 && hexDumpRowWidth(8) == 45 && hexDumpRowWidth(4) == 29
    );
    TEST_CHECK(joinedText(line(hexDump(std::string(16, 'a')), 0)).size() == hexDumpRowWidth(16));
    TEST_CHECK(hexDumpRowsFitting(78) == 16 && hexDumpRowsFitting(500) == 16);
    TEST_CHECK(hexDumpRowsFitting(77.9) == 12 && hexDumpRowsFitting(62) == 12);
    TEST_CHECK(hexDumpRowsFitting(61.9) == 8 && hexDumpRowsFitting(45) == 8);
    TEST_CHECK(hexDumpRowsFitting(44.9) == 4 && hexDumpRowsFitting(29) == 4);
    TEST_CHECK(hexDumpRowsFitting(10) == 4 && hexDumpRowsFitting(-1) == 4);
    TEST_CHECK(hexDumpRowsFitting(std::numeric_limits<double>::quiet_NaN()) == 4);
    TEST_CHECK(hexDumpRowsFitting(std::numeric_limits<double>::infinity()) == 16);
  }

  // A dump is laid out again with other rows from its own rows: the same bytes, the same cut.
  void testRowsAgain() {
    using luciole::formats::hexDumpAgain;
    using luciole::formats::hexDumpRowBytes;
    std::string bytes;
    for (int value = 0; value < 256 * 3 + 5; ++value) {
      bytes += static_cast<char>(value % 256);
    }
    const auto sixteen = hexDump(bytes);
    TEST_CHECK(hexDumpRowBytes(sixteen) == 16);
    const auto eight = hexDumpAgain(sixteen, 8);
    TEST_CHECK(eight.has_value() && hexDumpRowBytes(*eight) == 8);
    TEST_CHECK(eight->blocks[0].lines.size() == hexDump(bytes, 8).blocks[0].lines.size());
    for (std::size_t row = 0; row < eight->blocks[0].lines.size(); ++row) {
      TEST_CHECK(joinedText(line(*eight, row)) == joinedText(line(hexDump(bytes, 8), row)));
    }
    const auto twelve = hexDumpAgain(*eight, 12);
    TEST_CHECK(twelve.has_value() && hexDumpRowBytes(*twelve) == 12);
    TEST_CHECK(joinedText(twelve->blocks[0].lines.back()) == joinedText(hexDump(bytes, 12).blocks[0].lines.back()));
    // One short row, and a dump cut at its limit, which stays cut.
    const auto shortRow = hexDumpAgain(hexDump("abc"sv, 8), 16);
    TEST_CHECK(shortRow && joinedText(line(*shortRow, 0)) == joinedText(line(hexDump("abc"sv), 0)));
    const auto cut = hexDumpAgain(hexDump(std::string(kHexDumpBytes + 1, 'x')), 8);
    TEST_CHECK(cut && cut->bytesCapped && cut->blocks[0].lines.size() == kHexDumpBytes / 8);
    const auto empty = hexDumpAgain(hexDump({}), 8);
    TEST_CHECK(empty && empty->blocks.size() == 1 && empty->blocks[0].lines.empty());
    TEST_CHECK(hexDumpRowBytes(hexDump({})) == 0);
  }

  // Anything hexDump did not make is refused: other text, a row out of order, a damaged byte, a row too long.
  void testOtherDocumentsRefused() {
    using luciole::formats::hexDumpAgain;
    using luciole::formats::hexDumpRowBytes;
    StyledDocument prose;
    prose.blocks.emplace_back().lines.push_back({.runs = {{.text = "hello | world |"}}, .swatches = {}});
    TEST_CHECK(!hexDumpAgain(prose, 8) && hexDumpRowBytes(prose) == 0);
    TEST_CHECK(!hexDumpAgain(StyledDocument{}, 8));
    const auto edit = [](std::string_view bytes, std::size_t row, std::size_t at, std::string_view text) {
      auto document = hexDump(bytes);
      auto& runs = document.blocks[0].lines[row].runs;
      std::string joined = joinedText(document.blocks[0].lines[row]);
      joined.replace(at, text.size(), text);
      runs = {{.text = joined}};
      return document;
    };
    const auto twoRows = std::string(20, 'a');
    TEST_CHECK(hexDumpAgain(hexDump(twoRows), 8).has_value());
    TEST_CHECK(!hexDumpAgain(edit(twoRows, 1, 0, "00000020"), 8)); // offset out of order
    TEST_CHECK(!hexDumpAgain(edit(twoRows, 0, 10, "zz"), 8));      // not a hex byte
    TEST_CHECK(!hexDumpAgain(edit(twoRows, 0, 77, "a"), 8));       // the text column lost its bar
    TEST_CHECK(!hexDumpAgain(edit(twoRows, 0, 41, "  "), 8));      // a gap inside a full row
  }
} // namespace

int main() {
  testRows();
  testRoles();
  testEmpty();
  testCap();
  testNarrowRows();
  testRowFitting();
  testRowsAgain();
  testOtherDocumentsRefused();
  std::println("formats-hex-dump: all checks passed");
  return 0;
}
