#include "formats/file_kind.h"
#include "formats/line_tokens.h"
#include "formats/text_lines.h"
#include "formats/text_tokens.h"
#include "test_check.h"

#include <cstddef>
#include <optional>
#include <print>
#include <string_view>

namespace {
  using namespace luciole::formats;
  using namespace luciole::formats::tokenize;

  std::optional<TokenClass> classAt(const TextLine& line, std::size_t offset) {
    for (const auto& token : line) {
      if (offset >= token.offset && offset < token.offset + token.length) {
        return token.tokenClass;
      }
    }
    return std::nullopt;
  }

  TextLine tokens(std::string_view s, TextSyntax syntax, ScanState& state) {
    TextLine line;
    tokenizeLine({.s = s, .base = 0, .out = &line}, syntax, {}, state);
    return line;
  }

  void testPlain() {
    ScanState state;
    TEST_CHECK((tokens("hello", TextSyntax::Plain, state) == TextLine{{.offset = 0, .length = 5}}));
    TEST_CHECK(tokens("", TextSyntax::Markdown, state).empty());
    TEST_CHECK((tokens("a,b", TextSyntax::Delimited, state) == TextLine{{.offset = 0, .length = 3}}));
  }

  void testDiffHunks() {
    ScanState state;
    TEST_CHECK(classAt(tokens("diff --git a/x b/x", TextSyntax::Diff, state), 0) == TokenClass::Header);
    TEST_CHECK(classAt(tokens("@@ -1,2 +1,3 @@", TextSyntax::Diff, state), 0) == TokenClass::Hunk);
    TEST_CHECK(state.oldLeft == 2 && state.newLeft == 3);
    TEST_CHECK(classAt(tokens(" same", TextSyntax::Diff, state), 0) == TokenClass::Plain);
    TEST_CHECK(classAt(tokens("--- gone", TextSyntax::Diff, state), 0) == TokenClass::Removed);
    TEST_CHECK(classAt(tokens("+++ new", TextSyntax::Diff, state), 0) == TokenClass::Added);
    TEST_CHECK(classAt(tokens("+two", TextSyntax::Diff, state), 0) == TokenClass::Added);
    TEST_CHECK(state.oldLeft == 0 && state.newLeft == 0);
    // Outside a hunk the same prefixes are file headers.
    TEST_CHECK(classAt(tokens("--- a/x", TextSyntax::Diff, state), 0) == TokenClass::Header);
  }

  void testYaml() {
    ScanState state;
    const auto line = tokens("  - key: 'v' # c", TextSyntax::Yaml, state);
    TEST_CHECK(classAt(line, 0) == TokenClass::Plain && classAt(line, 2) == TokenClass::Punct);
    TEST_CHECK(classAt(line, 4) == TokenClass::Key && classAt(line, 7) == TokenClass::Punct);
    TEST_CHECK(classAt(line, 9) == TokenClass::String && classAt(line, 13) == TokenClass::Comment);
    TEST_CHECK(classAt(tokens("# note", TextSyntax::Yaml, state), 0) == TokenClass::Comment);
    TEST_CHECK(classAt(tokens("---", TextSyntax::Yaml, state), 0) == TokenClass::Punct);
  }

  void testIniAndJsonLines() {
    ScanState state;
    TEST_CHECK(classAt(tokens("[core]", TextSyntax::Ini, state), 0) == TokenClass::Tag);
    TEST_CHECK(classAt(tokens("; note", TextSyntax::Ini, state), 0) == TokenClass::Comment);
    TEST_CHECK(classAt(tokens("k = 1", TextSyntax::Ini, state), 0) == TokenClass::Key);
    const auto json = tokens(R"({"a": 1, "b": "x", "c": null})", TextSyntax::JsonLines, state);
    TEST_CHECK(classAt(json, 0) == TokenClass::Punct && classAt(json, 1) == TokenClass::Key);
    TEST_CHECK(classAt(json, 6) == TokenClass::Number && classAt(json, 14) == TokenClass::String);
    TEST_CHECK(classAt(json, 24) == TokenClass::Literal);
  }

  void testXmlCommentCarriesOver() {
    ScanState state;
    const auto open = tokens(R"(<a href="x">t <!-- c)", TextSyntax::Xml, state);
    TEST_CHECK(classAt(open, 0) == TokenClass::Punct && classAt(open, 1) == TokenClass::Tag);
    TEST_CHECK(classAt(open, 3) == TokenClass::Attr && classAt(open, 8) == TokenClass::String);
    TEST_CHECK(classAt(open, 14) == TokenClass::Comment && state.block.has_value());
    const auto closed = tokens("d --> e", TextSyntax::Xml, state);
    TEST_CHECK(classAt(closed, 0) == TokenClass::Comment && classAt(closed, 4) == TokenClass::Comment);
    TEST_CHECK(classAt(closed, 6) == TokenClass::Plain && !state.block);
  }
} // namespace

int main() {
  testPlain();
  testDiffHunks();
  testYaml();
  testIniAndJsonLines();
  testXmlCommentCarriesOver();
  std::println("formats line tokens checks passed");
  return 0;
}
