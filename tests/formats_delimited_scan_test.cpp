#include "formats/delimited_scan.h"
#include "formats/delimited_table.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;
  using namespace luciole::formats::delimited;

  void testSyntaxAndStart() {
    const Syntax tabs =
        syntaxOf({.delimiter = Delimiter::Tab, .quote = Quote::Single, .escape = QuoteEscape::Backslash});
    TEST_CHECK(tabs.delimiter == '\t' && tabs.quote == '\'' && tabs.backslash);
    const Syntax plain = syntaxOf({});
    TEST_CHECK(plain.delimiter == ',' && plain.quote == '"' && !plain.backslash);
    const std::string marked = std::string(kByteOrderMark) + "a,b";
    TEST_CHECK(contentStart(marked, {.byteOrderMark = true}) == 3);
    TEST_CHECK(contentStart(marked, {}) == 0);
    TEST_CHECK(contentStart("a,b", {.byteOrderMark = true}) == 0);
    TEST_CHECK(isLineBreak('\n') && isLineBreak('\r') && !isLineBreak(' '));
  }

  void testRecords() {
    constexpr std::string_view bytes = "a,\"b,c\",d\r\n\nx\ry";
    std::vector<Cell> cells;
    const auto first = scanRecord(bytes, 0, syntaxOf({}), cells, 2);
    TEST_CHECK(first.begin == 0 && first.contentEnd == 9 && first.end == 11);
    TEST_CHECK(first.fieldCount == 3 && first.terminator == Terminator::CrLf && !first.unterminated);
    TEST_CHECK((cells == std::vector<Cell>{{.begin = 0, .end = 1}, {.begin = 2, .end = 7, .quoted = true}}));
    cells.clear();
    const auto empty = scanRecord(bytes, 11, syntaxOf({}), cells, 8);
    TEST_CHECK(empty.fieldCount == 0 && empty.contentEnd == 11 && empty.end == 12 && cells.empty());
    TEST_CHECK(empty.terminator == Terminator::Lf);
    const auto cr = scanRecord(bytes, 12, syntaxOf({}), cells, 8);
    TEST_CHECK(cr.end == 14 && cr.terminator == Terminator::Cr);
    const auto last = scanRecord(bytes, 14, syntaxOf({}), cells, 8);
    TEST_CHECK(last.end == bytes.size() && last.terminator == Terminator::None && last.fieldCount == 1);
  }

  void testUnterminatedAndPastTheEnd() {
    std::vector<Cell> cells;
    const auto open = scanRecord("a,\"b\nc", 0, syntaxOf({}), cells, 8);
    TEST_CHECK(open.unterminated && open.end == 6 && open.fieldCount == 2);
    cells.clear();
    for (const std::size_t pos : {std::size_t{3}, std::size_t{4}, std::size_t{1000}}) {
      const auto past = scanRecord("abc", pos, syntaxOf({}), cells, 8);
      TEST_CHECK(past.begin == pos && past.contentEnd == pos && past.end == pos);
      TEST_CHECK(past.fieldCount == 0 && past.terminator == Terminator::None && cells.empty());
    }
    const auto none = scanRecord("", 0, syntaxOf({}), cells, 8);
    TEST_CHECK(none.end == 0 && none.fieldCount == 0);
  }
} // namespace

int main() {
  testSyntaxAndStart();
  testRecords();
  testUnterminatedAndPastTheEnd();
  std::println("formats delimited scan checks passed");
  return 0;
}
