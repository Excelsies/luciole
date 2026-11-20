#include "formats/text_lines.h"
#include "formats/text_tokens.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::formats;
  using namespace luciole::formats::tokenize;

  void testCodePoints() {
    constexpr std::string_view mixed = "a\xC3\xA9\xE2\x82\xACx";
    TEST_CHECK(codePoints(mixed, 10) == 4);
    TEST_CHECK(codePoints("abcdef", 2) == 3);
    TEST_CHECK(codePoints("\x80\x80", 10) == 2);
    TEST_CHECK(codePoints("", 10) == 0);
    TEST_CHECK(prefixBytes(mixed, 2) == 3);
    TEST_CHECK(prefixBytes(mixed, 3) == 6);
    TEST_CHECK(prefixBytes("abc", 9) == 3);
    TEST_CHECK(prefixBytes("\x80\x80\x80", 1) == 1);
  }

  void testWriter() {
    TextLine line;
    const LineWriter w{.s = "ab cd", .base = 10, .out = &line};
    w.add(0, 2, TokenClass::Plain);
    w.add(2, 3, TokenClass::Plain);
    w.add(4, 4, TokenClass::Key);
    w.tail(3).add(0, 2, TokenClass::String);
    TEST_CHECK(
        (line
         == TextLine{
             {.offset = 10, .length = 3, .tokenClass = TokenClass::Plain},
             {.offset = 13, .length = 2, .tokenClass = TokenClass::String}
         })
    );
    TextLine keys;
    const LineWriter k{.s = "ab", .base = 0, .out = &keys};
    k.add(0, 1, TokenClass::Key);
    k.add(1, 2, TokenClass::Key);
    TEST_CHECK(keys.size() == 2);
  }

  void testStringsNumbersAndSpace() {
    TEST_CHECK(isQuote('"') && isQuote('\'') && isQuote('`') && !isQuote('a'));
    TEST_CHECK(stringEnd("'ab\\'c' x", 0) == 7);
    TEST_CHECK(stringEnd("\"abc", 0) == 4);
    TEST_CHECK(stringEnd("ab", 2) == 2);
    TEST_CHECK(stringEnd("ab", 9) == 2);
    TEST_CHECK(isNumber("-12.5e+3") && isNumber("0x1F_ff") && isNumber("12_000") && isNumber("+7"));
    TEST_CHECK(!isNumber("1.") && !isNumber("0x") && !isNumber("e5") && !isNumber("1e") && !isNumber(""));
    TEST_CHECK(!isNumber("12a"));
    TEST_CHECK(skipJsonSpace(" \t\r\n x", 0) == 5);
    TEST_CHECK(skipJsonSpace("x", 3) == 3);
  }

  void testBlocks() {
    TextLine line;
    ScanState state;
    const LineWriter w{.s = "x */ y", .base = 0, .out = &line};
    TEST_CHECK(inBlock(w, 1, state) == 1 && line.empty());
    state.block = Block{.end = "*/", .tokenClass = TokenClass::Comment};
    TEST_CHECK(inBlock(w, 0, state) == 4 && !state.block);
    TEST_CHECK((line == TextLine{{.offset = 0, .length = 4, .tokenClass = TokenClass::Comment}}));
    state.block = Block{.end = "-->", .tokenClass = TokenClass::Comment};
    TEST_CHECK(inBlock(w, 4, state) == 6 && state.block.has_value());
  }

  void testSplitAndCut() {
    const auto split = splitLines("a\r\nb\rc\n");
    TEST_CHECK(split.total == 3 && split.lines.size() == 3);
    TEST_CHECK(split.lines[0].begin == 0 && split.lines[0].end == 1);
    TEST_CHECK(split.lines[1].begin == 3 && split.lines[1].end == 4);
    TEST_CHECK(split.lines[2].begin == 5 && split.lines[2].end == 6);
    TEST_CHECK(splitLines("").total == 1 && splitLines("").lines.size() == 1);
    const std::string many(kMaxTextLines + 5, '\n');
    const auto capped = splitLines(many);
    TEST_CHECK(capped.total == kMaxTextLines + 5 && capped.lines.size() == kMaxTextLines);
    const std::string wide(kMaxLineWidth + 50, 'x');
    TEST_CHECK(splitLines(wide).lines[0].end == kMaxLineWidth + 1);

    TextLine line{{.offset = 0, .length = kMaxLineWidth + 50, .tokenClass = TokenClass::Plain}};
    TEST_CHECK(cutWidth(wide, line));
    TEST_CHECK(line.size() == 2 && line[0].length == kMaxLineWidth);
    TEST_CHECK(line[1].tokenClass == TokenClass::More && line[1].offset == kMaxLineWidth);
    TextLine shortLine{{.offset = 0, .length = 3, .tokenClass = TokenClass::Plain}};
    TEST_CHECK(!cutWidth(wide, shortLine) && shortLine.size() == 1);
    // A token that lies past the text counts as empty.
    TextLine stray{{.offset = 1'000'000, .length = 5, .tokenClass = TokenClass::Plain}};
    TEST_CHECK(!cutWidth("abc", stray) && stray.size() == 1);
  }
} // namespace

int main() {
  testCodePoints();
  testWriter();
  testStringsNumbersAndSpace();
  testBlocks();
  testSplitAndCut();
  std::println("formats text lines checks passed");
  return 0;
}
