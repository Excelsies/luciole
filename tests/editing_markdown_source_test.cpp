#include "editing/markdown_source.h"
#include "editing/source_lines.h"
#include "formats/file_kind.h"
#include "formats/styled_text.h"
#include "test_check.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using luciole::editing::fenceAfter;
  using luciole::editing::LineRange;
  using luciole::editing::MarkdownFence;
  using luciole::editing::markdownRuns;
  using luciole::editing::SourceStyles;
  using luciole::formats::StyledRun;
  using luciole::formats::StyleRole;

  std::size_t codePoints(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
      count += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return count;
  }

  std::string joined(const std::vector<StyledRun>& runs) {
    std::string out;
    for (const auto& run : runs) {
      out += run.text;
    }
    return out;
  }

  // The run holding the code point at `column`.
  StyledRun runAt(const std::vector<StyledRun>& runs, std::size_t column) {
    for (const auto& run : runs) {
      const auto width = codePoints(run.text);
      if (column < width) {
        return run;
      }
      column -= width;
    }
    return {};
  }

  // Checks the role of every column in [first, last].
  void checkSpan(std::string_view line, std::size_t first, std::size_t last, StyleRole role, MarkdownFence fence = {}) {
    const auto runs = markdownRuns(line, fence);
    TEST_CHECK(joined(runs) == line);
    for (auto column = first; column <= last; ++column) {
      if (runAt(runs, column).role != role) {
        std::println(stderr, "'{}' column {}", line, column);
      }
      TEST_CHECK(runAt(runs, column).role == role);
    }
  }

  void testHeadings() {
    checkSpan("# Title", 0, 6, StyleRole::Heading);
    checkSpan("###### six", 0, 9, StyleRole::Heading);
    checkSpan("   ## indented", 0, 13, StyleRole::Heading);
    checkSpan("#nospace", 0, 7, StyleRole::Plain);
    checkSpan("####### seven", 0, 12, StyleRole::Plain);
  }

  void testFenceState() {
    constexpr MarkdownFence kNone{};
    constexpr MarkdownFence kTicks{.ch = '`', .count = 3};
    TEST_CHECK(fenceAfter("```py", kNone) == kTicks);
    TEST_CHECK(fenceAfter("  ```", kTicks) == kNone);
    TEST_CHECK(fenceAfter("    ```", kTicks) == kTicks);
    TEST_CHECK(fenceAfter("``` x", kTicks) == kTicks);
    TEST_CHECK(fenceAfter("~~~", kTicks) == kTicks);
    TEST_CHECK((fenceAfter("~~~~", kNone) == MarkdownFence{.ch = '~', .count = 4}));
    TEST_CHECK(fenceAfter("~~~", MarkdownFence{.ch = '~', .count = 4}) == MarkdownFence({.ch = '~', .count = 4}));
    TEST_CHECK(fenceAfter("```a```", kNone) == kNone);
    TEST_CHECK(fenceAfter("text", kNone) == kNone);
    TEST_CHECK(fenceAfter("text", kTicks) == kTicks);
  }

  void testFenceLines() {
    constexpr MarkdownFence kTicks{.ch = '`', .count = 3};
    checkSpan("```cpp", 0, 2, StyleRole::Punctuation);
    checkSpan("```cpp", 3, 5, StyleRole::InlineCode);
    checkSpan("int /* *x* `y`", 0, 13, StyleRole::InlineCode, kTicks);
    checkSpan("# not a heading", 0, 14, StyleRole::InlineCode, kTicks);
    checkSpan("  ```", 2, 4, StyleRole::Punctuation, kTicks);
    checkSpan("```a``` b", 0, 6, StyleRole::InlineCode);
  }

  void testQuotesListsAndRules() {
    checkSpan("> > quoted *em*", 0, 0, StyleRole::Quote);
    checkSpan("> > quoted *em*", 2, 2, StyleRole::Quote);
    checkSpan("> > quoted *em*", 4, 9, StyleRole::Plain);
    checkSpan("> > quoted *em*", 11, 14, StyleRole::Emphasis);
    checkSpan("- item", 0, 0, StyleRole::Punctuation);
    checkSpan("- item", 2, 5, StyleRole::Plain);
    checkSpan("  12. num", 2, 4, StyleRole::Punctuation);
    checkSpan("  12. num", 6, 8, StyleRole::Plain);
    checkSpan("> - quoted item", 2, 2, StyleRole::Punctuation);
    checkSpan("-not a list", 0, 10, StyleRole::Plain);
    checkSpan("> # quoted heading", 2, 17, StyleRole::Heading);
    for (const std::string_view rule : {"---", "* * *", "___", "===", "  ==="}) {
      checkSpan(rule, 0, rule.size() - 1, StyleRole::Punctuation);
    }
  }

  void testCodeSpans() {
    checkSpan("use `a*b*c` here", 4, 10, StyleRole::InlineCode);
    checkSpan("use `a*b*c` here", 12, 15, StyleRole::Plain);
    checkSpan("``a ` b`` x", 0, 8, StyleRole::InlineCode);
    checkSpan("``a ` b`` x", 10, 10, StyleRole::Plain);
    checkSpan("a ` b ``", 0, 7, StyleRole::Plain);
  }

  void testEmphasis() {
    const std::string_view line = "*em* _us_ **st** __un__ ***both***";
    checkSpan(line, 0, 3, StyleRole::Emphasis);
    checkSpan(line, 5, 8, StyleRole::Emphasis);
    checkSpan(line, 10, 15, StyleRole::Strong);
    checkSpan(line, 17, 22, StyleRole::Strong);
    checkSpan(line, 24, 33, StyleRole::Strong);
    const auto runs = markdownRuns(line, {});
    TEST_CHECK(runAt(runs, 1).italic && !runAt(runs, 1).bold);
    TEST_CHECK(runAt(runs, 12).bold && !runAt(runs, 12).italic);
    TEST_CHECK(runAt(runs, 27).bold && runAt(runs, 27).italic);
    TEST_CHECK(!runAt(runs, 4).italic && !runAt(runs, 4).bold);
    checkSpan("a snake_case_name here", 0, 21, StyleRole::Plain);
    checkSpan("2 * 3 * 4", 0, 8, StyleRole::Plain);
    checkSpan("**unclosed", 0, 9, StyleRole::Plain);
    checkSpan("x*y*z", 1, 3, StyleRole::Emphasis);
    checkSpan("x*y*z", 4, 4, StyleRole::Plain);
    checkSpan("\\*not\\* em", 0, 9, StyleRole::Plain);
    // A closer is a whole marker run: a run of another length is skipped, and a styled range never ends inside one.
    checkSpan("*a **b** c*", 0, 10, StyleRole::Emphasis);
    checkSpan("***a**", 0, 5, StyleRole::Plain);
    checkSpan("****a****", 0, 8, StyleRole::Strong);
    checkSpan("**a*b**", 0, 6, StyleRole::Strong);
    // Underscores inside words, closers after white space, escaped brackets and one-letter schemes stay plain.
    checkSpan("a_b_ c", 0, 5, StyleRole::Plain);
    checkSpan("*a *", 0, 3, StyleRole::Plain);
    checkSpan("[a\\](b)", 0, 6, StyleRole::Plain);
    checkSpan("<a:b>", 0, 4, StyleRole::Plain);
  }

  void testLinks() {
    const std::string_view line = "[text](http://x.y/(a)) and ![img](p.png) <https://a.b> [x] y";
    checkSpan(line, 0, 21, StyleRole::Link);
    checkSpan(line, 22, 26, StyleRole::Plain);
    checkSpan(line, 27, 39, StyleRole::Link);
    checkSpan(line, 41, 53, StyleRole::Link);
    checkSpan(line, 54, 59, StyleRole::Plain);
    checkSpan("<not a link> [a](b", 0, 17, StyleRole::Plain);
    checkSpan("[a [b] c](d) e", 0, 11, StyleRole::Link);
  }

  void testCodePointsKept() {
    for (const std::string_view line :
         {"# Tïtle ✓", "*é* `ü` [ñ](ö) **\U0001F600**", "> - é", "``` é", "\t- tab *a*\t`b`"}) {
      TEST_CHECK(joined(markdownRuns(line, {})) == line);
      TEST_CHECK(joined(markdownRuns(line, {.ch = '`', .count = 3})) == line);
    }
  }

  std::string repeat(std::string_view unit, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += unit;
    }
    return out;
  }

  // 16 KiB lines of unclosed or alternating markers stay quick, also with the only closer at the end.
  void testLongLinesStayQuick() {
    double slowest = 0.0;
    std::vector<std::string> lines;
    for (const std::string_view unit :
         {"*a ", "[", "`a``b```c", "_a ", "[a](", "**a ", "<a", "\\", "![", " `", "<a:", "\\``", "*a**"}) {
      lines.push_back(repeat(unit, (16 * 1024) / unit.size()));
    }
    lines.push_back(repeat("[", 16000) + "](x)");
    lines.push_back(repeat("[a](", 4000) + ")");
    lines.push_back(repeat("[a]", 5000) + "(x)");
    for (const auto& line : lines) {
      const auto start = std::clock();
      const auto runs = markdownRuns(line, {});
      const double ms = 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
      TEST_CHECK(joined(runs) == line);
      slowest = std::max(slowest, ms);
      TEST_CHECK(ms < 50.0);
    }
    std::println("long lines: slowest {:.3f} ms", slowest);
  }

  void testFenceCarryAndEdits() {
    std::vector<std::string> lines{"text", "```", "code *x*", "```", "after", "more"};
    SourceStyles styles{luciole::formats::fileKind("a.md"), lines.size(), [&lines](std::size_t i) {
                          return std::string_view{lines[i]};
                        }};
    const auto roleOf = [&styles](std::size_t line, std::size_t column) {
      const auto styled = styles.line(line);
      TEST_CHECK(styled.has_value());
      return runAt(styled->runs, column).role;
    };
    TEST_CHECK(roleOf(2, 5) == StyleRole::InlineCode);
    TEST_CHECK(roleOf(4, 0) == StyleRole::Plain);
    TEST_CHECK(roleOf(5, 0) == StyleRole::Plain);

    lines.erase(lines.begin() + 3);
    TEST_CHECK((styles.edited(3, 1, 0) == LineRange{.first = 3, .count = 2}));
    TEST_CHECK(roleOf(3, 0) == StyleRole::InlineCode);
    TEST_CHECK(roleOf(4, 0) == StyleRole::InlineCode);

    lines.insert(lines.begin() + 3, "```");
    TEST_CHECK((styles.edited(3, 0, 1) == LineRange{.first = 3, .count = 3}));
    TEST_CHECK(roleOf(3, 0) == StyleRole::Punctuation);
    TEST_CHECK(roleOf(4, 0) == StyleRole::Plain);

    lines[2] = "other code";
    TEST_CHECK((styles.edited(2, 1, 1) == LineRange{.first = 2, .count = 1}));
    TEST_CHECK(roleOf(2, 0) == StyleRole::InlineCode);
    lines[0] = "```";
    TEST_CHECK((styles.edited(0, 1, 1) == LineRange{.first = 0, .count = 6}));
    TEST_CHECK(roleOf(1, 0) == StyleRole::Punctuation);
    TEST_CHECK(roleOf(2, 0) == StyleRole::Plain);
    TEST_CHECK(roleOf(4, 0) == StyleRole::InlineCode);
    TEST_CHECK(roleOf(5, 0) == StyleRole::InlineCode);
  }
} // namespace

int main() {
  testHeadings();
  testFenceState();
  testFenceLines();
  testQuotesListsAndRules();
  testCodeSpans();
  testEmphasis();
  testLinks();
  testCodePointsKept();
  testLongLinesStayQuick();
  testFenceCarryAndEdits();
  std::println("editing_markdown_source_test: ok");
  return 0;
}
