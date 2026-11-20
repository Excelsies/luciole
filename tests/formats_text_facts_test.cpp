#include "formats/text_facts.h"
#include "test_check.h"

#include <array>
#include <print>
#include <string>
#include <string_view>

namespace {
  using luciole::formats::editorText;
  using luciole::formats::fileBytes;
  using luciole::formats::LineEnding;
  using luciole::formats::textFacts;
  using namespace std::string_view_literals;

  constexpr std::string_view kMark = "\xEF\xBB\xBF";

  void testEachStyle() {
    const auto lf = textFacts("one\ntwo\n");
    TEST_CHECK(lf.lineEnding == LineEnding::Lf && !lf.mixedEndings && lf.finalNewline && lf.validUtf8);
    TEST_CHECK(!lf.byteOrderMark);
    const auto crlf = textFacts("one\r\ntwo\r\nthree");
    TEST_CHECK(crlf.lineEnding == LineEnding::CrLf && !crlf.mixedEndings && !crlf.finalNewline);
    const auto cr = textFacts("one\rtwo\r");
    TEST_CHECK(cr.lineEnding == LineEnding::Cr && !cr.mixedEndings && cr.finalNewline);
    const auto none = textFacts("a single line");
    TEST_CHECK(none.lineEnding == LineEnding::None && !none.mixedEndings && !none.finalNewline);
    const auto blank = textFacts("\r\n\r\n\r\n");
    TEST_CHECK(blank.lineEnding == LineEnding::CrLf && !blank.mixedEndings && blank.finalNewline);
  }

  void testMixedEndings() {
    const auto mostlyCrlf = textFacts("a\r\nb\r\nc\nd\r\n");
    TEST_CHECK(mostlyCrlf.lineEnding == LineEnding::CrLf && mostlyCrlf.mixedEndings && mostlyCrlf.finalNewline);
    const auto mostlyCr = textFacts("a\rb\rc\n");
    TEST_CHECK(mostlyCr.lineEnding == LineEnding::Cr && mostlyCr.mixedEndings);
    // Ties go to LF, then CRLF, then CR.
    const auto tieLf = textFacts("a\nb\r\n");
    TEST_CHECK(tieLf.lineEnding == LineEnding::Lf && tieLf.mixedEndings);
    const auto tieCrlf = textFacts("a\rb\r\n");
    TEST_CHECK(tieCrlf.lineEnding == LineEnding::CrLf && tieCrlf.mixedEndings);
    // A CR right before a CRLF is a lone CR followed by a CRLF.
    const auto doubled = textFacts("a\r\r\n");
    TEST_CHECK(doubled.mixedEndings && doubled.finalNewline);
    TEST_CHECK(editorText("a\r\r\nb\rc\n") == "a\n\nb\nc\n");
  }

  void testLoneTrailingCr() {
    const auto onlyCr = textFacts("last line\r");
    TEST_CHECK(onlyCr.lineEnding == LineEnding::Cr && !onlyCr.mixedEndings && onlyCr.finalNewline);
    TEST_CHECK(editorText("last line\r") == "last line\n");
    const auto afterLf = textFacts("a\nb\r");
    TEST_CHECK(afterLf.lineEnding == LineEnding::Lf && afterLf.mixedEndings && afterLf.finalNewline);
    const auto bare = textFacts("\r");
    TEST_CHECK(bare.lineEnding == LineEnding::Cr && bare.finalNewline && bare.validUtf8);
  }

  void testEmptyAndMark() {
    const auto empty = textFacts("");
    TEST_CHECK(empty.validUtf8 && !empty.byteOrderMark && empty.lineEnding == LineEnding::None);
    TEST_CHECK(!empty.mixedEndings && !empty.finalNewline);
    TEST_CHECK(editorText("").empty() && fileBytes("", empty).empty());

    const auto markOnly = textFacts(kMark);
    TEST_CHECK(markOnly.validUtf8 && markOnly.byteOrderMark && !markOnly.finalNewline);
    TEST_CHECK(markOnly.lineEnding == LineEnding::None);
    TEST_CHECK(editorText(kMark).empty());
    TEST_CHECK(fileBytes("", markOnly) == kMark);

    const std::string marked = std::string(kMark) + "caf\xC3\xA9\r\n";
    const auto withContent = textFacts(marked);
    TEST_CHECK(withContent.byteOrderMark && withContent.validUtf8 && withContent.lineEnding == LineEnding::CrLf);
    TEST_CHECK(withContent.finalNewline);
    TEST_CHECK(editorText(marked) == "caf\xC3\xA9\n");
    TEST_CHECK(fileBytes("caf\xC3\xA9\nmore\n", withContent) == std::string(kMark) + "caf\xC3\xA9\r\nmore\r\n");

    // Only a leading mark is a mark; a second one is content.
    const std::string twice = std::string(kMark) + std::string(kMark) + "x";
    TEST_CHECK(textFacts(twice).byteOrderMark && editorText(twice) == std::string(kMark) + "x");
    TEST_CHECK(!textFacts("x\xEF\xBB\xBF").byteOrderMark);
    // A partial mark is neither a mark nor valid UTF-8.
    const auto partial = textFacts("\xEF\xBB");
    TEST_CHECK(!partial.byteOrderMark && !partial.validUtf8);
    TEST_CHECK(editorText("\xEF\xBB") == "\xEF\xBB");
  }

  void testValidUtf8() {
    const std::array valid = {
        "plain ascii"sv,           "\xC2\xA9 two bytes"sv,   "\xE2\x82\xAC three bytes"sv,
        "\xF0\x9F\x98\x80 four"sv, "\xF4\x8F\xBF\xBF max"sv, "\xED\x9F\xBF below surrogates"sv,
        "\xEE\x80\x80 above"sv,    "nul\0inside"sv,          "\xEF\xBF\xBF noncharacter"sv,
    };
    for (const auto text : valid) {
      TEST_CHECK(textFacts(text).validUtf8);
    }
  }

  void testInvalidUtf8AtEachPosition() {
    const std::array invalid = {
        // Lead byte position.
        "\x80 stray continuation at the start"sv,
        "middle \xBF stray continuation"sv,
        "\xC0\xAF overlong two bytes"sv,
        "\xC1\xBF overlong two bytes"sv,
        "\xF5\x80\x80\x80 lead above U+10FFFF"sv,
        "\xFF never valid"sv,
        "\xFE never valid"sv,
        // Second byte position.
        "\xC3( second byte not a continuation"sv,
        "\xE0\x80\x80 overlong three bytes"sv,
        "\xE0\x9F\xBF overlong three bytes"sv,
        "\xED\xA0\x80 high surrogate"sv,
        "\xED\xBF\xBF low surrogate"sv,
        "\xF0\x80\x80\x80 overlong four bytes"sv,
        "\xF0\x8F\xBF\xBF overlong four bytes"sv,
        "\xF4\x90\x80\x80 above U+10FFFF"sv,
        // Third and fourth byte positions.
        "\xE2\x82( third byte"sv,
        "\xF0\x9F\x98( fourth byte"sv,
        "\xF0\x9F( third of four"sv,
        // Truncated at the end of the range.
        "ends in a lead \xC3"sv,
        "ends after two of three \xE2\x82"sv,
        "ends after three of four \xF0\x9F\x98"sv,
        // Right after a mark.
        "\xEF\xBB\xBF\x80"sv,
    };
    for (const auto text : invalid) {
      const auto facts = textFacts(text);
      if (facts.validUtf8) {
        std::println(stderr, "accepted invalid UTF-8 starting with byte {:#04x}", static_cast<unsigned char>(text[0]));
      }
      TEST_CHECK(!facts.validUtf8);
    }
    // Invalid bytes do not change the other facts.
    const auto latin1 = textFacts("caf\xE9\r\nna\xEFve\r\n");
    TEST_CHECK(!latin1.validUtf8 && latin1.lineEnding == LineEnding::CrLf && !latin1.mixedEndings);
  }

  void testConversions() {
    TEST_CHECK(editorText("a\r\nb\r\n") == "a\nb\n");
    TEST_CHECK(editorText("a\rb") == "a\nb");
    TEST_CHECK(editorText("a\nb") == "a\nb");
    const auto cr = textFacts("x\r");
    TEST_CHECK(fileBytes("one\ntwo\n\n", cr) == "one\rtwo\r\r");
    const auto crlf = textFacts("x\r\n");
    TEST_CHECK(fileBytes("one\ntwo", crlf) == "one\r\ntwo");
    // Lines added to a file without a line break use LF.
    const auto none = textFacts("x");
    TEST_CHECK(fileBytes("x\ny", none) == "x\ny");
    // The final newline is the text's own: none is appended or removed.
    TEST_CHECK(fileBytes("no end", crlf) == "no end");
    // Mixed files write the dominant ending.
    const auto mixed = textFacts("a\r\nb\r\nc\n");
    TEST_CHECK(fileBytes(editorText("a\r\nb\r\nc\n"), mixed) == "a\r\nb\r\nc\r\n");
  }

  void testRoundTrip() {
    const std::array files = {
        ""sv,
        "\n"sv,
        "\r"sv,
        "\r\n"sv,
        "\n\n\n"sv,
        "\r\r\r"sv,
        "\r\n\r\n"sv,
        "no newline at all"sv,
        "a\nb\nc"sv,
        "a\r\nb\r\n\r\n"sv,
        "classic mac\rlines\r"sv,
        "\xEF\xBB\xBF"sv,
        "\xEF\xBB\xBF\r\n"sv,
        "\xEF\xBB\xBF\xEF\xBB\xBF two marks\n"sv,
        "\xEF\xBB\xBFmark\rand cr"sv,
        "tab\tand nul\0 and bell \a\n"sv,
        "latin-1 caf\xE9\r\n"sv,
        "\xFF\xFE\r\nutf-16 lookalike\r\n"sv,
        "trailing spaces   \n   \n"sv,
        "\xF0\x9F\x98\x80\r\n\xE2\x82\xAC\r\n"sv,
        "\xC3"sv,
    };
    for (const auto bytes : files) {
      const auto facts = textFacts(bytes);
      TEST_CHECK(!facts.mixedEndings);
      const auto text = editorText(bytes);
      TEST_CHECK(!text.contains('\r'));
      TEST_CHECK(fileBytes(text, facts) == bytes);
    }
    const std::array mixed = {"a\nb\r"sv, "a\r\nb\n"sv, "a\rb\r\n"sv, "\r\r\n"sv};
    for (const auto bytes : mixed) {
      TEST_CHECK(textFacts(bytes).mixedEndings);
      TEST_CHECK(!editorText(bytes).contains('\r'));
    }
  }

  void testLargeInput() {
    std::string bytes;
    constexpr std::size_t kLines = 200000;
    bytes.reserve(kLines * 6);
    for (std::size_t i = 0; i < kLines; ++i) {
      bytes += "line\r\n";
    }
    const auto facts = textFacts(bytes);
    TEST_CHECK(facts.lineEnding == LineEnding::CrLf && !facts.mixedEndings && facts.validUtf8);
    const auto text = editorText(bytes);
    TEST_CHECK(text.size() == kLines * 5);
    TEST_CHECK(fileBytes(text, facts) == bytes);
  }
} // namespace

int main() {
  testEachStyle();
  testMixedEndings();
  testLoneTrailingCr();
  testEmptyAndMark();
  testValidUtf8();
  testInvalidUtf8AtEachPosition();
  testConversions();
  testRoundTrip();
  testLargeInput();
  return 0;
}
