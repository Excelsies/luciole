#include "formats/code_tokens.h"
#include "formats/file_kind.h"
#include "formats/language_keywords.h"
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

  // The class of the token holding byte `offset`, if any.
  std::optional<TokenClass> classAt(const TextLine& line, std::size_t offset) {
    for (const auto& token : line) {
      if (offset >= token.offset && offset < token.offset + token.length) {
        return token.tokenClass;
      }
    }
    return std::nullopt;
  }

  TextLine code(std::string_view s, ScanState& state, const CodeOptions& options) {
    TextLine line;
    scanCode({.s = s, .base = 0, .out = &line}, state, options);
    return line;
  }

  void testCodeLine() {
    ScanState state;
    const CodeOptions cpp{.blocks = true, .keywords = keywords(Language::Cpp)};
    const auto line = code("int x = 42; // hi", state, cpp);
    TEST_CHECK(classAt(line, 0) == TokenClass::Keyword);
    TEST_CHECK(classAt(line, 4) == TokenClass::Plain);
    TEST_CHECK(classAt(line, 8) == TokenClass::Number);
    TEST_CHECK(classAt(line, 10) == TokenClass::Plain);
    TEST_CHECK(classAt(line, 12) == TokenClass::Comment && classAt(line, 16) == TokenClass::Comment);
    const auto strings = code(R"("a\"b" true)", state, cpp);
    TEST_CHECK(classAt(strings, 0) == TokenClass::String && classAt(strings, 5) == TokenClass::String);
    TEST_CHECK(classAt(strings, 6) == TokenClass::Plain && classAt(strings, 7) == TokenClass::Literal);
  }

  void testBlockCommentCarriesOver() {
    ScanState state;
    const CodeOptions cpp{.blocks = true};
    const auto open = code("a /* b", state, cpp);
    TEST_CHECK(classAt(open, 2) == TokenClass::Comment && classAt(open, 5) == TokenClass::Comment);
    TEST_CHECK(state.block.has_value());
    const auto closed = code("c */ d", state, cpp);
    TEST_CHECK(classAt(closed, 0) == TokenClass::Comment && classAt(closed, 3) == TokenClass::Comment);
    TEST_CHECK(classAt(closed, 5) == TokenClass::Plain && !state.block);
  }

  void testHashComments() {
    ScanState state;
    const CodeOptions hash{.style = CommentStyle::Hash};
    TEST_CHECK(classAt(code("a #b", state, hash), 2) == TokenClass::Comment);
    TEST_CHECK(classAt(code("a#b", state, hash), 1) == TokenClass::Plain);
  }

  void testKeyValue() {
    ScanState state;
    const CodeOptions ini{.style = CommentStyle::Semi, .family = Family::Ini};
    TextLine line;
    keyValue({.s = "name \t= 'v' ;c", .base = 0, .out = &line}, 6, ini, state);
    TEST_CHECK((line.front() == Token{.offset = 0, .length = 4, .tokenClass = TokenClass::Key}));
    TEST_CHECK(classAt(line, 4) == TokenClass::Plain && classAt(line, 6) == TokenClass::Punct);
    TEST_CHECK(classAt(line, 8) == TokenClass::String && classAt(line, 12) == TokenClass::Comment);
    TextLine spaced;
    keyValue({.s = "a\xC2\xA0=1", .base = 0, .out = &spaced}, 3, ini, state);
    TEST_CHECK((spaced.front() == Token{.offset = 0, .length = 1, .tokenClass = TokenClass::Key}));
    TEST_CHECK(classAt(spaced, 1) == TokenClass::Plain && classAt(spaced, 4) == TokenClass::Number);
    TextLine on;
    keyValue({.s = "k=on", .base = 0, .out = &on}, 1, ini, state);
    TEST_CHECK(classAt(on, 2) == TokenClass::Literal);
    // A separator past the end of the line makes the whole line the key.
    TextLine past;
    keyValue({.s = "abc", .base = 0, .out = &past}, 9, ini, state);
    TEST_CHECK((past == TextLine{{.offset = 0, .length = 3, .tokenClass = TokenClass::Key}}));
  }
} // namespace

int main() {
  testCodeLine();
  testBlockCommentCarriesOver();
  testHashComments();
  testKeyValue();
  std::println("formats code tokens checks passed");
  return 0;
}
