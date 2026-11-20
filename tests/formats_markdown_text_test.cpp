#include "formats/markdown.h"
#include "formats/markdown_text.h"
#include "test_check.h"

#include <print>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::formats::markdown;

  void testText() {
    TEST_CHECK(charAt("ab", 1) == 'b' && charAt("ab", 2) == '\0');
    TEST_CHECK(trimmed("\xC2\xA0 a b \n") == "a b");
    TEST_CHECK(isBlank(" \t\xE3\x80\x80") && isBlank("") && !isBlank(" a"));
    TEST_CHECK(indentOf("  \tx") == 2);
  }

  void testUtf8() {
    TEST_CHECK(sequenceLength("\xF0\x9F\x99\x82", 0) == 4);
    TEST_CHECK(sequenceLength("\xF0\x9F", 0) == 1);
    TEST_CHECK(sequenceLength("\xC0\x80", 0) == 1);
    TEST_CHECK(sequenceLength("a", 3) == 0);
    TEST_CHECK(unitsOf(4) == 2 && unitsOf(3) == 1);
    TEST_CHECK(unitCount("a\xF0\x9F\x99\x82\xC3\xA9") == 4);
    TEST_CHECK(
        cutPoint(
            "a\xF0\x9F\x99\x82"
            "b",
            2
        )
        == 1
    );
    TEST_CHECK(
        cutPoint(
            "a\xF0\x9F\x99\x82"
            "b",
            3
        )
        == 5
    );
    TEST_CHECK(cutPoint("abc", 10) == 3);
  }

  void testLines() {
    const Line line{.text = "  hello", .mappedFrom = 0, .origin = 10};
    TEST_CHECK(originAt(line, 2) == 12);
    const auto rest = sliceLine(line, 2);
    TEST_CHECK(rest.text == "hello" && rest.origin == 12);
    const auto trimmedLine = trimLine({.text = " a ", .mappedFrom = 0, .origin = 5});
    TEST_CHECK(trimmedLine.text == "a" && trimmedLine.origin == 6);
    TEST_CHECK(trimStart({.text = " a ", .mappedFrom = 0, .origin = 5}).text == "a ");
    // Bytes before mappedFrom were made by the parser, here a tab turned into spaces.
    const Line expanded{.text = "    x", .mappedFrom = 3, .origin = 20};
    TEST_CHECK(originAt(expanded, 1) == kNoOrigin && originAt(expanded, 4) == 21);
    TEST_CHECK(originAt({.text = "ab"}, 1) == kNoOrigin);
  }

  void testPiecesAndSources() {
    Piece piece;
    piece.append("ab", 4);
    piece.append("cd", 6);
    TEST_CHECK(piece.text == "abcd" && piece.origin == 4);
    piece.append("e", 9);
    TEST_CHECK(piece.origin == kNoOrigin);
    Source source;
    source.append(' ', 0);
    source.append('a', 1);
    source.append('b', 2);
    source.append('c', kNoOrigin);
    source.append(' ', 4);
    TEST_CHECK(source.piece(1, 3).text == "ab" && source.piece(1, 3).origin == 1);
    TEST_CHECK(source.piece(2, 4).origin == kNoOrigin);
    TEST_CHECK(source.piece(2, 2).text.empty());
    trimSource(source);
    TEST_CHECK(source.text == "abc" && (source.origin == std::vector<std::size_t>{1, 2, kNoOrigin}));
  }

  void testStore() {
    Document document;
    document.text = "hello world";
    document.sourceSize = document.text.size();
    Context context{.document = document, .expanded = {}};
    TEST_CHECK((store(context, {.text = "world", .origin = 6}) == Span{.offset = 6, .length = 5}));
    TEST_CHECK((store(context, {.text = "xyz", .origin = 6}) == Span{.offset = 11, .length = 3}));
    TEST_CHECK(document.text == "hello worldxyz");
    // A claim past the input is not trusted, even when the bytes there match.
    TEST_CHECK((store(context, {.text = "xyz", .origin = 11}) == Span{.offset = 14, .length = 3}));
    TEST_CHECK((store(context, {}) == Span{}));
    std::vector<Pending> nodes;
    nodes.push_back({.text = {.text = "hello", .origin = 0}});
    const auto range = commit(context, std::move(nodes));
    TEST_CHECK((range == Range{.first = 0, .count = 1}));
    TEST_CHECK((document.runs[0].text == Span{.offset = 0, .length = 5}));
    std::vector<int> target{1};
    TEST_CHECK((appendAll(target, std::vector<int>{2, 3}) == Range{.first = 1, .count = 2}));
    TEST_CHECK((target == std::vector<int>{1, 2, 3}));
  }
} // namespace

int main() {
  testText();
  testUtf8();
  testLines();
  testPiecesAndSources();
  testStore();
  std::println("formats markdown text checks passed");
  return 0;
}
