#include "cpu_budget.h"
#include "editing/text_buffer.h"
#include "test_check.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace {
  using luciole::editing::EditResult;
  using luciole::editing::EraseUnit;
  using luciole::editing::IndentUnit;
  using luciole::editing::kEditByteLimit;
  using luciole::editing::kEditLineBytes;
  using luciole::editing::LineSpan;
  using luciole::editing::TextBuffer;
  using luciole::editing::TextPosition;
  using luciole::editing::TextSelection;

  // The speed check takes about 0.3 s of processor time at -O0 and under the sanitizers.
  constexpr double kSpeedBoundSeconds = 2.0;

  TextBuffer make(std::string_view text) {
    auto buffer = TextBuffer::create(text);
    TEST_CHECK(buffer.has_value());
    return std::move(*buffer);
  }

  void testRoundTrip() {
    const auto withFinal = make("one\ntwo\n");
    TEST_CHECK(withFinal.lineCount() == 3);
    TEST_CHECK(withFinal.line(1) == "two");
    TEST_CHECK(withFinal.line(2).empty());
    TEST_CHECK(withFinal.text() == "one\ntwo\n");
    TEST_CHECK(withFinal.byteCount() == 8);
    const auto without = make("one\ntwo");
    TEST_CHECK(without.lineCount() == 2);
    TEST_CHECK(without.text() == "one\ntwo");
    const auto empty = make("");
    TEST_CHECK(empty.lineCount() == 1 && empty.text().empty());
    const auto blank = make("\n\n");
    TEST_CHECK(blank.lineCount() == 3 && blank.text() == "\n\n");
    const auto unicode = make("caf\xC3\xA9\n\xF0\x9F\x98\x80");
    TEST_CHECK(unicode.text() == "caf\xC3\xA9\n\xF0\x9F\x98\x80");
    TEST_CHECK((unicode.selection() == TextSelection{}));
    TEST_CHECK(!unicode.modified() && unicode.revision() == 0);
  }

  void testRefusals() {
    const auto invalid = TextBuffer::create("ok\n\xC3(");
    TEST_CHECK(!invalid.has_value() && invalid.error() == "The text is not valid UTF-8.");
    const auto overlong = TextBuffer::create("\xC0\xAF");
    TEST_CHECK(!overlong.has_value() && overlong.error() == "The text is not valid UTF-8.");
    const auto cr = TextBuffer::create("one\r\ntwo");
    TEST_CHECK(!cr.has_value() && cr.error() == "The text contains a carriage return.");
    const auto nul = TextBuffer::create(std::string_view("a\0b", 3));
    TEST_CHECK(!nul.has_value() && nul.error() == "The text contains a NUL byte.");

    const std::string longLine(kEditLineBytes + 1, 'x');
    const auto wide = TextBuffer::create("short\n" + longLine);
    TEST_CHECK(!wide.has_value() && wide.error() == "Line 2 is longer than 16 KiB.");
    TEST_CHECK(TextBuffer::create(std::string(kEditLineBytes, 'x')).has_value());

    std::string big;
    while (big.size() + 1000 <= kEditByteLimit) {
      big += std::string(999, 'y') + '\n';
    }
    big += std::string(kEditByteLimit - big.size(), 'z');
    TEST_CHECK(big.size() == kEditByteLimit && TextBuffer::create(big).has_value());
    big += 'z';
    const auto huge = TextBuffer::create(big);
    TEST_CHECK(!huge.has_value() && huge.error() == "The text is larger than 1 MiB.");
  }

  void testClamp() {
    // "a" + e acute (2 bytes) + euro sign (3 bytes) + grinning face (4 bytes)
    auto buffer = make("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\nxy");
    TEST_CHECK((buffer.clamp({0, 2}) == TextPosition{0, 1}));
    TEST_CHECK((buffer.clamp({0, 3}) == TextPosition{0, 3}));
    TEST_CHECK((buffer.clamp({0, 5}) == TextPosition{0, 3}));
    TEST_CHECK((buffer.clamp({0, 8}) == TextPosition{0, 6}));
    TEST_CHECK((buffer.clamp({0, 10}) == TextPosition{0, 10}));
    TEST_CHECK((buffer.clamp({0, 99}) == TextPosition{0, 10}));
    TEST_CHECK((buffer.clamp({7, 0}) == TextPosition{1, 2}));
    buffer.setSelection({{0, 4}, {9, 9}});
    TEST_CHECK((buffer.selection() == TextSelection{{0, 3}, {1, 2}}));
  }

  void testTextBetween() {
    // Line 1 is "tw" + e acute (bytes 2 and 3) + "o".
    const auto buffer = make("one\ntw\xC3\xA9o\nthree");
    TEST_CHECK(buffer.textBetween({0, 1}, {0, 3}) == "ne");
    TEST_CHECK(buffer.textBetween({0, 1}, {2, 2}) == "ne\ntw\xC3\xA9o\nth");
    TEST_CHECK(buffer.textBetween({2, 2}, {0, 1}) == "ne\ntw\xC3\xA9o\nth");
    TEST_CHECK(buffer.textBetween({1, 2}, {1, 2}).empty());
    TEST_CHECK(buffer.textBetween({0, 0}, {2, 5}) == "one\ntw\xC3\xA9o\nthree");
    // Positions are clamped like clamp() does: inside a code point, past a line end, past the last line.
    TEST_CHECK(buffer.textBetween({1, 3}, {1, 5}) == "\xC3\xA9o");
    TEST_CHECK(buffer.textBetween({0, 99}, {1, 1}) == "\nt");
    TEST_CHECK(buffer.textBetween({1, 4}, {9, 9}) == "o\nthree");
    // A selection's text, whichever way it was made.
    auto selected = make("alpha beta\ngamma");
    selected.setSelection({{1, 3}, {0, 6}});
    TEST_CHECK(selected.textBetween(selected.selection().anchor, selected.selection().cursor) == "beta\ngam");
    TEST_CHECK(selected.revision() == 0 && !selected.modified());
  }

  // Checks that `result` changed the `expected` lines.
  void checkSpan(const EditResult& result, LineSpan expected) {
    TEST_CHECK(result.has_value() && result->has_value());
    TEST_CHECK(**result == expected);
  }

  void checkUnchanged(const EditResult& result) { TEST_CHECK(result.has_value() && !result->has_value()); }

  void checkRefused(const EditResult& result, std::string_view reason) {
    TEST_CHECK(!result.has_value() && result.error() == reason);
  }

  TextBuffer at(std::string_view text, TextSelection selection) {
    auto buffer = make(text);
    buffer.setSelection(selection);
    return buffer;
  }

  TextBuffer at(std::string_view text, TextPosition cursor) { return at(text, {cursor, cursor}); }

  void testInsertText() {
    auto plain = at("ab", TextPosition{0, 1});
    checkSpan(plain.insertText("X", 0), {0, 1, 1});
    TEST_CHECK(plain.text() == "aXb" && (plain.selection() == TextSelection{{0, 2}, {0, 2}}));
    TEST_CHECK(plain.revision() == 1 && plain.modified());

    auto across = at("one\ntwo\nthree", {{2, 2}, {0, 1}});
    checkSpan(across.insertText("Z", 0), {0, 3, 1});
    TEST_CHECK(across.text() == "oZree" && (across.selection() == TextSelection{{0, 2}, {0, 2}}));

    auto pasted = at("ab", TextPosition{0, 1});
    checkSpan(pasted.insertText("p\r\nq\rr\n", 0), {0, 1, 4});
    TEST_CHECK(pasted.text() == "ap\nq\nr\nb" && (pasted.selection() == TextSelection{{3, 0}, {3, 0}}));

    auto erased = at("abc", {{0, 0}, {0, 2}});
    checkSpan(erased.insertText("", 0), {0, 1, 1});
    TEST_CHECK(erased.text() == "c");
    auto nothing = at("abc", TextPosition{0, 1});
    checkUnchanged(nothing.insertText("", 0));
    TEST_CHECK(nothing.revision() == 0);
    auto same = at("abc", {{0, 0}, {0, 1}});
    checkUnchanged(same.insertText("a", 0));
    TEST_CHECK(same.revision() == 0 && (same.selection() == TextSelection{{0, 1}, {0, 1}}));
  }

  void testInsertRefusals() {
    auto buffer = at("ab", {{0, 0}, {0, 1}});
    checkRefused(buffer.insertText("\xFF", 0), "The inserted text is not valid UTF-8.");
    checkRefused(buffer.insertText(std::string_view("x\0", 2), 0), "The inserted text contains a NUL byte.");
    TEST_CHECK(buffer.text() == "ab" && (buffer.selection() == TextSelection{{0, 0}, {0, 1}}));
    TEST_CHECK(buffer.revision() == 0 && !buffer.modified());

    auto wide = at("a\n" + std::string(kEditLineBytes - 1, 'x'), TextPosition{1, 3});
    checkRefused(wide.insertText("yz", 0), "The edit would make line 2 longer than 16 KiB.");
    checkSpan(wide.insertText("y", 0), {1, 1, 1});
    checkRefused(
        wide.insertText("\n\n" + std::string(kEditLineBytes + 1, 'w'), 0),
        "The edit would make line 4 longer than 16 KiB."
    );
    auto halves = at(std::string(9000, 'p') + '\n' + std::string(9000, 'q'), TextPosition{1, 0});
    checkRefused(halves.eraseBackward(EraseUnit::Cluster, 0), "The edit would make line 1 longer than 16 KiB.");
    TEST_CHECK(halves.lineCount() == 2 && (halves.selection() == TextSelection{{1, 0}, {1, 0}}));

    std::string full;
    while (full.size() + 1000 <= kEditByteLimit) {
      full += std::string(999, 'y') + '\n';
    }
    full += std::string(kEditByteLimit - full.size(), 'z');
    auto big = at(full, TextPosition{0, 0});
    checkRefused(big.insertText("a", 0), "The edit would make the text larger than 1 MiB.");
    checkRefused(big.newline(0), "The edit would make the text larger than 1 MiB.");
    big.setSelection({{0, 0}, {0, 1}});
    checkSpan(big.insertText("a", 0), {0, 1, 1});
    TEST_CHECK(big.byteCount() == kEditByteLimit);
    // Three bytes, a line break among them, replaced by three bytes: the size stays at the limit.
    big.setSelection({{0, 998}, {1, 1}});
    checkSpan(big.insertText("xyz", 0), {0, 2, 1});
    TEST_CHECK(big.byteCount() == kEditByteLimit && big.line(0).substr(997, 5) == "yxyzy");
  }

  void testNewline() {
    auto indented = at("    foo", TextPosition{0, 7});
    checkSpan(indented.newline(0), {0, 1, 2});
    TEST_CHECK(indented.text() == "    foo\n    " && (indented.selection() == TextSelection{{1, 4}, {1, 4}}));
    auto inside = at("    foo", TextPosition{0, 2});
    checkSpan(inside.newline(0), {0, 1, 2});
    TEST_CHECK(inside.text() == "  \n    foo" && (inside.selection() == TextSelection{{1, 2}, {1, 2}}));
    auto tabbed = at("\tx\ny", {{0, 2}, {1, 1}});
    checkSpan(tabbed.newline(0), {0, 2, 2});
    TEST_CHECK(tabbed.text() == "\tx\n\t" && (tabbed.selection() == TextSelection{{1, 1}, {1, 1}}));
  }

  // "a", e + combining acute, then "b c" on the next line.
  constexpr std::string_view kErase = "ae\xCC\x81\nb cd";

  void testEraseBackward() {
    auto cluster = at(kErase, TextPosition{0, 4});
    checkSpan(cluster.eraseBackward(EraseUnit::Cluster, 0), {0, 1, 1});
    TEST_CHECK(cluster.text() == "a\nb cd" && (cluster.selection() == TextSelection{{0, 1}, {0, 1}}));
    auto join = at(kErase, TextPosition{1, 0});
    checkSpan(join.eraseBackward(EraseUnit::Cluster, 0), {0, 2, 1});
    TEST_CHECK(
        join.text()
            == "ae\xCC\x81"
               "b cd"
        && (join.selection() == TextSelection{{0, 4}, {0, 4}})
    );
    auto start = at(kErase, TextPosition{0, 0});
    checkUnchanged(start.eraseBackward(EraseUnit::Cluster, 0));
    auto word = at(kErase, TextPosition{1, 4});
    checkSpan(word.eraseBackward(EraseUnit::Word, 0), {1, 1, 1});
    TEST_CHECK(word.text() == "ae\xCC\x81\nb " && (word.selection() == TextSelection{{1, 2}, {1, 2}}));
    auto edge = at(kErase, TextPosition{1, 3});
    checkSpan(edge.eraseBackward(EraseUnit::LineEdge, 0), {1, 1, 1});
    TEST_CHECK(edge.text() == "ae\xCC\x81\nd");
    checkSpan(edge.eraseBackward(EraseUnit::LineEdge, 0), {0, 2, 1});
    TEST_CHECK(
        edge.text()
        == "ae\xCC\x81"
           "d"
    );
    auto selected = at(kErase, {{1, 2}, {0, 1}});
    checkSpan(selected.eraseBackward(EraseUnit::Word, 0), {0, 2, 1});
    TEST_CHECK(selected.text() == "acd" && (selected.selection() == TextSelection{{0, 1}, {0, 1}}));
  }

  void testEraseForward() {
    auto cluster = at(kErase, TextPosition{0, 1});
    checkSpan(cluster.eraseForward(EraseUnit::Cluster, 0), {0, 1, 1});
    TEST_CHECK(cluster.text() == "a\nb cd" && (cluster.selection() == TextSelection{{0, 1}, {0, 1}}));
    auto join = at(kErase, TextPosition{0, 4});
    checkSpan(join.eraseForward(EraseUnit::Cluster, 0), {0, 2, 1});
    TEST_CHECK(
        join.text()
        == "ae\xCC\x81"
           "b cd"
    );
    auto end = at(kErase, TextPosition{1, 4});
    checkUnchanged(end.eraseForward(EraseUnit::Cluster, 0));
    auto word = at(kErase, TextPosition{1, 1});
    checkSpan(word.eraseForward(EraseUnit::Word, 0), {1, 1, 1});
    TEST_CHECK(word.text() == "ae\xCC\x81\nb");
    auto edge = at(kErase, TextPosition{0, 1});
    checkSpan(edge.eraseForward(EraseUnit::LineEdge, 0), {0, 1, 1});
    TEST_CHECK(edge.text() == "a\nb cd");
    checkSpan(edge.eraseForward(EraseUnit::LineEdge, 0), {0, 2, 1});
    TEST_CHECK(edge.text() == "ab cd" && (edge.selection() == TextSelection{{0, 1}, {0, 1}}));
    auto selected = at(kErase, {{0, 1}, {1, 2}});
    checkSpan(selected.eraseForward(EraseUnit::Cluster, 0), {0, 2, 1});
    TEST_CHECK(selected.text() == "acd");
  }

  constexpr IndentUnit kTab{.tab = true};
  constexpr IndentUnit kTwo{.spaces = 2};
  constexpr IndentUnit kFour{.spaces = 4};

  void testIndent() {
    auto cursor = at("ab", TextPosition{0, 1});
    checkSpan(cursor.indent(kFour, 0), {0, 1, 1});
    TEST_CHECK(cursor.text() == "a    b" && (cursor.selection() == TextSelection{{0, 5}, {0, 5}}));
    auto word = at("ab", {{0, 0}, {0, 1}});
    checkSpan(word.indent(kTab, 0), {0, 1, 1});
    TEST_CHECK(word.text() == "\tb" && (word.selection() == TextSelection{{0, 1}, {0, 1}}));
    auto lines = at("a\n\nb\nc", {{2, 1}, {0, 1}});
    checkSpan(lines.indent(kTwo, 0), {0, 3, 3});
    TEST_CHECK(lines.text() == "  a\n\n  b\nc" && (lines.selection() == TextSelection{{2, 3}, {0, 3}}));
    auto toNextLine = at("a\n\nb\nc", {{0, 0}, {2, 0}});
    checkSpan(toNextLine.indent(kTab, 0), {0, 2, 2});
    TEST_CHECK(toNextLine.text() == "\ta\n\nb\nc" && (toNextLine.selection() == TextSelection{{0, 0}, {2, 0}}));
    auto zero = at("ab", TextPosition{0, 1});
    checkRefused(zero.indent(IndentUnit{.spaces = 0}, 0), "The indent unit needs at least one space.");
    checkRefused(zero.outdent(IndentUnit{.spaces = 0}, 0), "The indent unit needs at least one space.");
    TEST_CHECK(zero.text() == "ab");
  }

  void testOutdent() {
    auto lines = at("      a\n  b\n\tc\nd", {{0, 7}, {3, 1}});
    checkSpan(lines.outdent(kFour, 0), {0, 4, 4});
    TEST_CHECK(lines.text() == "  a\nb\n\tc\nd" && (lines.selection() == TextSelection{{0, 3}, {3, 1}}));
    auto tabs = at("\t\ta\n\tb\nc", {{0, 0}, {2, 1}});
    checkSpan(tabs.outdent(kTab, 0), {0, 3, 3});
    TEST_CHECK(tabs.text() == "\ta\nb\nc" && (tabs.selection() == TextSelection{{0, 0}, {2, 1}}));
    auto single = at("    ab", {{0, 4}, {0, 6}});
    checkSpan(single.outdent(kFour, 0), {0, 1, 1});
    TEST_CHECK(single.text() == "ab" && (single.selection() == TextSelection{{0, 0}, {0, 2}}));
    auto flat = at("a\nb", {{0, 0}, {1, 1}});
    checkUnchanged(flat.outdent(kTwo, 0));
    TEST_CHECK(flat.revision() == 0);

    auto spaces = at("a    b", TextPosition{0, 5});
    checkSpan(spaces.outdent(kFour, 0), {0, 1, 1});
    TEST_CHECK(spaces.text() == "ab" && (spaces.selection() == TextSelection{{0, 1}, {0, 1}}));
    auto fewer = at("a  b", TextPosition{0, 3});
    checkSpan(fewer.outdent(kFour, 0), {0, 1, 1});
    TEST_CHECK(fewer.text() == "ab");
    // Six spaces before the cursor lose one unit of four.
    auto more = at("a      b", TextPosition{0, 7});
    checkSpan(more.outdent(kFour, 0), {0, 1, 1});
    TEST_CHECK(more.text() == "a  b" && (more.selection() == TextSelection{{0, 3}, {0, 3}}));
    auto tab = at("a\tb", TextPosition{0, 2});
    checkSpan(tab.outdent(kTab, 0), {0, 1, 1});
    TEST_CHECK(tab.text() == "ab");
    auto none = at("ab", TextPosition{0, 1});
    checkUnchanged(none.outdent(kFour, 0));
    auto noTab = at("ab", TextPosition{0, 1});
    checkUnchanged(noTab.outdent(kTab, 0));
    TEST_CHECK(noTab.text() == "ab" && noTab.revision() == 0);
  }

  void testDetectIndent() {
    TEST_CHECK(detectIndent(make("a\nb\n")) == kFour);
    TEST_CHECK(detectIndent(make("a\n\tb\n\t\tc\n  d")).tab);
    TEST_CHECK(!detectIndent(make("a\n\tb\n  c\n  d")).tab);
    TEST_CHECK(detectIndent(make("a\n  b\n    c\n  d")) == kTwo);
    TEST_CHECK(detectIndent(make("if\n    x\n        y\n")) == kFour);
    TEST_CHECK(detectIndent(make("a\n        b\nc\n        d")) == IndentUnit{.spaces = 8});
    TEST_CHECK(detectIndent(make("a\n\n    b\n   \n    c\nd")) == kFour);
    TEST_CHECK(detectIndent(make("a\n   b\n      c")) == kFour);
    // One step of 2 and one of 4: the tie prefers 4.
    TEST_CHECK(detectIndent(make("a\n  b\n      c")) == kFour);
    // A tab line ends the run of space lines, so no step of 2 is measured across it.
    TEST_CHECK(detectIndent(make("a\n    b\n\tc\n  d\ne")) == kFour);
  }

  // 20,000 lines of 50 bytes: 1,019,999 bytes, leaving the edits room under kEditByteLimit.
  void testSpeed() {
    std::string text;
    for (std::size_t line = 0; line < 20'000; ++line) {
      text += std::format("    value{:05}=compute(alpha,beta,gamma)+offset_x;", line);
      text += line + 1 < 20'000 ? "\n" : "";
    }
    TEST_CHECK(text.size() == 1'019'999);
    auto buffer = make(text);
    const auto start = std::clock();
    for (std::uint64_t edit = 0; edit < 10'000; ++edit) {
      buffer.setSelection(
          {{(edit * 7919) % 20'000, 4 + ((edit * 13) % 40)}, {(edit * 7919) % 20'000, 4 + ((edit * 13) % 40)}}
      );
      checkSpan(buffer.insertText("q", edit * 2000), {(edit * 7919) % 20'000, 1, 1});
    }
    for (std::uint64_t edit = 0; edit < 1'000; ++edit) {
      const TextPosition cursor{(edit * 104'729) % 20'000, 20};
      buffer.setSelection({cursor, cursor});
      checkSpan(buffer.newline(20'000'000 + edit), {cursor.line, 1, 2});
    }
    const auto seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    std::println(
        "speed: 10,000 inserts and 1,000 newlines on 1,019,999 bytes took {:.3f} s of processor time", seconds
    );
    TEST_CHECK(buffer.lineCount() == 21'000 && buffer.byteCount() == 1'019'999 + 10'000 + 5'000);
    TEST_CHECK(TEST_WITHIN_BUDGET(seconds, kSpeedBoundSeconds));
  }
} // namespace

int main() {
  testRoundTrip();
  testRefusals();
  testClamp();
  testTextBetween();
  testInsertText();
  testInsertRefusals();
  testNewline();
  testEraseBackward();
  testEraseForward();
  testIndent();
  testOutdent();
  testDetectIndent();
  testSpeed();
  std::println("editing_text_buffer_test: ok");
  return 0;
}
