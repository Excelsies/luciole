#include "editing/source_lines.h"
#include "formats/file_kind.h"
#include "formats/styled_text.h"
#include "test_check.h"

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  // Processor time for 1 MiB in 20,000 lines of C++, bounds that hold in debug and sanitizer builds. Creation scans
  // nothing; the first line asked for scans the lines before it.
  constexpr double kCreateBoundMs = 1.0;
  constexpr double kScanBoundMs = 150.0;
  constexpr double kEditBoundMs = 1.0;

  using luciole::editing::SourceStyles;
  using luciole::formats::fileKind;
  using luciole::formats::StyledLine;
  using luciole::formats::StyleRole;

  std::size_t codePoints(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
      count += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return count;
  }

  SourceStyles stylesOf(std::string_view name, const std::vector<std::string>& lines) {
    return {fileKind(name), lines.size(), [&lines](std::size_t i) { return std::string_view{lines[i]}; }};
  }

  // The role of the code point at `column`; Plain past the end.
  StyleRole roleAt(const StyledLine& line, std::size_t column) {
    for (const auto& run : line.runs) {
      const auto width = codePoints(run.text);
      if (column < width) {
        return run.role;
      }
      column -= width;
    }
    return StyleRole::Plain;
  }

  StyledLine lineOf(SourceStyles& styles, std::size_t index) {
    auto line = styles.line(index);
    TEST_CHECK(line.has_value());
    return line.value_or(StyledLine{});
  }

  bool sameLine(const StyledLine& a, const StyledLine& b) { return a.runs == b.runs && a.swatches == b.swatches; }

  // Every line matches a fresh scan of the same lines.
  void checkMatchesFresh(SourceStyles& styles, std::string_view name, const std::vector<std::string>& lines) {
    auto fresh = stylesOf(name, lines);
    TEST_CHECK(styles.lineCount() == lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      TEST_CHECK(sameLine(lineOf(styles, i), lineOf(fresh, i)));
    }
  }

  void testEditsOpenAndCloseComment() {
    std::vector<std::string> lines(10, "int a;");
    auto styles = stylesOf("a.cpp", lines);
    using Range = luciole::editing::LineRange;
    checkMatchesFresh(styles, "a.cpp", lines);

    lines[2] = "int /* open";
    TEST_CHECK((styles.edited(2, 1, 1) == Range{.first = 2, .count = 8}));
    TEST_CHECK(roleAt(lineOf(styles, 5), 0) == StyleRole::Comment);
    checkMatchesFresh(styles, "a.cpp", lines);

    lines[6] = "x */ int b;";
    TEST_CHECK((styles.edited(6, 1, 1) == Range{.first = 6, .count = 4}));
    TEST_CHECK(roleAt(lineOf(styles, 5), 0) == StyleRole::Comment);
    TEST_CHECK(roleAt(lineOf(styles, 6), 5) == StyleRole::Keyword);
    TEST_CHECK(roleAt(lineOf(styles, 7), 0) == StyleRole::Keyword);
    checkMatchesFresh(styles, "a.cpp", lines);

    // Edits that leave the next line's state alone stop at the edited line.
    lines[8] = "int c;";
    TEST_CHECK((styles.edited(8, 1, 1) == Range{.first = 8, .count = 1}));
    lines[4] = "still";
    TEST_CHECK((styles.edited(4, 1, 1) == Range{.first = 4, .count = 1}));
    TEST_CHECK(roleAt(lineOf(styles, 4), 0) == StyleRole::Comment);

    // Removing the opener turns lines 3 to 5 (formerly 4 to 6) back into code, then stops.
    lines.erase(lines.begin() + 2);
    TEST_CHECK((styles.edited(2, 1, 0) == Range{.first = 2, .count = 4}));
    TEST_CHECK(roleAt(lineOf(styles, 2), 0) == StyleRole::Keyword);
    TEST_CHECK(roleAt(lineOf(styles, 3), 0) == StyleRole::Plain);
    checkMatchesFresh(styles, "a.cpp", lines);

    // Inserted lines, a split line, and a removal that changes nothing after it.
    lines.insert(lines.begin() + 1, {"int x;", "/* y */"});
    TEST_CHECK((styles.edited(1, 0, 2) == Range{.first = 1, .count = 2}));
    lines[0] = "int";
    lines.insert(lines.begin() + 1, " a;");
    TEST_CHECK((styles.edited(0, 1, 2) == Range{.first = 0, .count = 2}));
    lines.erase(lines.begin() + 1);
    TEST_CHECK((styles.edited(1, 1, 0) == Range{.first = 1, .count = 0}));
    checkMatchesFresh(styles, "a.cpp", lines);
  }

  // The span stops at the last line asked for: lines past it are scanned only when asked for.
  void testEditsPastLinesAskedFor() {
    std::vector<std::string> lines(10, "int a;");
    auto styles = stylesOf("a.cpp", lines);
    using Range = luciole::editing::LineRange;
    for (std::size_t i = 0; i < 4; ++i) {
      TEST_CHECK(roleAt(lineOf(styles, i), 0) == StyleRole::Keyword);
    }
    lines[2] = "int /* open";
    TEST_CHECK((styles.edited(2, 1, 1) == Range{.first = 2, .count = 2}));
    TEST_CHECK(roleAt(lineOf(styles, 3), 0) == StyleRole::Comment);
    lines[7] = "x */";
    TEST_CHECK((styles.edited(7, 1, 1) == Range{.first = 7, .count = 1}));
    lines.erase(lines.begin() + 3, lines.begin() + 6);
    TEST_CHECK((styles.edited(3, 3, 0) == Range{.first = 3, .count = 0}));
    checkMatchesFresh(styles, "a.cpp", lines);
  }

  struct Edit {
    std::size_t first = 0;
    std::size_t removed = 0;
    std::vector<std::string> inserted;
  };

  // After each edit every line matches a fresh scan, and lines outside the returned span keep their old style.
  void checkEdits(std::string_view name, std::vector<std::string> lines, const std::vector<Edit>& edits) {
    auto styles = stylesOf(name, lines);
    for (const auto& edit : edits) {
      std::vector<StyledLine> before;
      before.reserve(lines.size());
      for (std::size_t i = 0; i < lines.size(); ++i) {
        before.push_back(lineOf(styles, i));
      }
      const auto from = lines.begin() + static_cast<std::ptrdiff_t>(edit.first);
      lines.erase(from, from + static_cast<std::ptrdiff_t>(edit.removed));
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(edit.first), edit.inserted.begin(), edit.inserted.end());
      const auto span = styles.edited(edit.first, edit.removed, edit.inserted.size());
      TEST_CHECK(span.has_value() && span->first == edit.first && span->count >= edit.inserted.size());
      checkMatchesFresh(styles, name, lines);
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i < span->first) {
          TEST_CHECK(sameLine(lineOf(styles, i), before[i]));
        } else if (i >= span->first + span->count) {
          TEST_CHECK(sameLine(lineOf(styles, i), before[i - edit.inserted.size() + edit.removed]));
        }
      }
    }
  }

  void testEditSequences() {
    checkEdits(
        "a.cpp", {"int a = 1;", "/* doc", " * more", " */", "char* s = \"x\";", "int b; // c", "return 0;"},
        {{.first = 0, .removed = 1, .inserted = {"int a = /* 1;"}},
         {.first = 3, .removed = 1, .inserted = {}},
         {.first = 2, .removed = 0, .inserted = {" */"}},
         {.first = 4, .removed = 2, .inserted = {"x", "y \"", "z"}},
         {.first = 7, .removed = 1, .inserted = {"/* tail"}},
         {.first = 8, .removed = 0, .inserted = {"a", "b"}},
         {.first = 10, .removed = 0, .inserted = {}},
         {.first = 0, .removed = 10, .inserted = {"all new"}},
         {.first = 0, .removed = 1, .inserted = {}},
         {.first = 0, .removed = 0, .inserted = {"/*", "*/"}}}
    );
    checkEdits(
        "a.py", {"a = '''", "b", "'''", "c = 1"},
        {{.first = 2, .removed = 1, .inserted = {"still"}}, {.first = 3, .removed = 0, .inserted = {"'''"}}}
    );
    checkEdits(
        "a.diff", {"@@ -1,2 +1,2 @@", "-a", "-b", "+c", "+d", "--- e"},
        {{.first = 0, .removed = 1, .inserted = {"@@ -1,3 +1,2 @@"}}, {.first = 1, .removed = 1, .inserted = {}}}
    );
    checkEdits(
        "a.xml", {"<a", "b='1'>", "<c/>", "<!-- x", "-->"},
        {{.first = 1, .removed = 1, .inserted = {"b='1'"}}, {.first = 3, .removed = 2, .inserted = {"<d/>"}}}
    );
    // Removals that change no state after them, with every line cached.
    checkEdits(
        "a.cpp", {"int a;", "int b;", "int c;", "int d;", "int e;"},
        {{.first = 1, .removed = 1, .inserted = {}}, {.first = 1, .removed = 2, .inserted = {"int x;"}}}
    );
  }

  void testErrors() {
    std::vector<std::string> lines{"ok", "bad \xC3", "two\nlines"};
    auto styles = stylesOf("a.cpp", lines);
    using luciole::editing::StyleError;
    TEST_CHECK(styles.line(3).error_or(StyleError::EditOutOfRange) == StyleError::LineOutOfRange);
    TEST_CHECK(styles.line(1).error_or(StyleError::EditOutOfRange) == StyleError::InvalidLine);
    TEST_CHECK(styles.line(2).error_or(StyleError::EditOutOfRange) == StyleError::InvalidLine);
    TEST_CHECK(styles.edited(4, 0, 0).error_or(StyleError::LineOutOfRange) == StyleError::EditOutOfRange);
    TEST_CHECK(styles.edited(1, 3, 1).error_or(StyleError::LineOutOfRange) == StyleError::EditOutOfRange);
    TEST_CHECK(styles.lineCount() == 3);
    TEST_CHECK(styles.line(0).has_value());

    // An edit that would leave more than kMaxSourceLines lines is refused and changes nothing.
    using luciole::editing::kMaxSourceLines;
    std::vector<std::string> code{"int a;", "/* b", "c */"};
    auto open = stylesOf("a.cpp", code);
    const auto last = lineOf(open, 2);
    constexpr auto kHuge = std::numeric_limits<std::size_t>::max();
    for (const std::size_t inserted : {kMaxSourceLines - 1, kMaxSourceLines, kHuge / 2, kHuge}) {
      TEST_CHECK(open.edited(0, 1, inserted).error_or(StyleError::LineOutOfRange) == StyleError::EditOutOfRange);
      TEST_CHECK(open.lineCount() == 3);
      TEST_CHECK(sameLine(lineOf(open, 2), last));
    }
    SourceStyles full{fileKind("a.txt"), 1, [](std::size_t) { return std::string_view{"x"}; }};
    TEST_CHECK(full.edited(1, 0, kMaxSourceLines - 1).has_value());
    TEST_CHECK(full.lineCount() == kMaxSourceLines);
  }

  // At most kCachedStyledLines lines are kept; a full cache drops the line asked for least recently, and a cached line
  // is not read again.
  void testCacheBound() {
    using luciole::editing::kCachedStyledLines;
    std::vector<std::string> lines(1000, "int a;");
    std::size_t reads = 0;
    SourceStyles styles{fileKind("a.cpp"), lines.size(), [&lines, &reads](std::size_t i) {
                          ++reads;
                          return std::string_view{lines[i]};
                        }};
    for (std::size_t i = 0; i < lines.size(); ++i) {
      TEST_CHECK(roleAt(lineOf(styles, i), 0) == StyleRole::Keyword);
      TEST_CHECK(styles.cachedLines() == std::min(i + 1, kCachedStyledLines));
    }
    const auto oldest = lines.size() - kCachedStyledLines;
    lineOf(styles, oldest);
    lineOf(styles, 0);
    const auto before = reads;
    lineOf(styles, oldest);
    TEST_CHECK(reads == before);
    lineOf(styles, oldest + 1);
    TEST_CHECK(reads > before);
    lines[3] = "/*";
    // Lines 3 to 999 change, so only line 0 stays cached.
    TEST_CHECK((styles.edited(3, 1, 1) == luciole::editing::LineRange{.first = 3, .count = 997}));
    TEST_CHECK(styles.cachedLines() == 1);
    for (std::size_t i = 0; i < 10; ++i) {
      TEST_CHECK(roleAt(lineOf(styles, i), 0) == (i < 3 ? StyleRole::Keyword : StyleRole::Comment));
    }
    TEST_CHECK(styles.cachedLines() == 10);
  }

  double millisecondsSince(std::clock_t start) {
    return 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
  }

  // A 1 MiB C++ file of 20,000 lines, with block comments and strings.
  std::vector<std::string> largeSource() {
    constexpr std::size_t kLines = 20000;
    constexpr std::size_t kBytes = 1024 * 1024;
    std::vector<std::string> lines;
    std::size_t total = 0;
    for (std::size_t i = 0; i < kLines; ++i) {
      std::string line = i % 50 == 0 ? "/* block comment "
          : i % 50 == 3              ? "   end */ int x;"
                                     : "  auto v = f(\"s\", 42);";
      const auto want = (kBytes - total) / (kLines - i) - 1;
      line.resize(std::max(line.size(), want), ' ');
      total += line.size() + 1;
      lines.push_back(std::move(line));
    }
    return lines;
  }

  void testSpeed() {
    auto lines = largeSource();
    std::size_t bytes = 0;
    for (const auto& line : lines) {
      bytes += line.size() + 1;
    }
    TEST_CHECK(lines.size() == 20000 && bytes == 1024 * 1024);
    const auto created = std::clock();
    auto styles = stylesOf("a.cpp", lines);
    const auto createMs = millisecondsSince(created);

    const auto screen = std::clock();
    for (std::size_t i = 10000; i < 10060; ++i) {
      TEST_CHECK(!lineOf(styles, i).runs.empty());
    }
    const auto screenMs = millisecondsSince(screen);
    const auto end = std::clock();
    TEST_CHECK(!lineOf(styles, lines.size() - 1).runs.empty());
    const auto endMs = millisecondsSince(end);

    lines[10010] = "  int changed = 1; /* note */ return 2;";
    const auto edit = std::clock();
    const auto span = styles.edited(10010, 1, 1);
    const auto editMs = millisecondsSince(edit);
    TEST_CHECK((span == luciole::editing::LineRange{.first = 10010, .count = 1}));

    lines[10010] = "  int changed = 1; /* opens";
    const auto opening = std::clock();
    const auto opened = styles.edited(10010, 1, 1);
    const auto openMs = millisecondsSince(opening);
    TEST_CHECK((opened == luciole::editing::LineRange{.first = 10010, .count = 41}));
    std::println(
        "speed: create {:.3f} ms, first 60 lines at line 10,000 {:.3f} ms, then the last line {:.3f} ms, "
        "edit {:.3f} ms, edit opening a comment {:.3f} ms",
        createMs, screenMs, endMs, editMs, openMs
    );
    TEST_CHECK(createMs < kCreateBoundMs);
    TEST_CHECK(screenMs < kScanBoundMs);
    TEST_CHECK(endMs < kScanBoundMs);
    TEST_CHECK(editMs < kEditBoundMs);
    TEST_CHECK(openMs < kEditBoundMs);
  }
} // namespace

int main() {
  testEditsOpenAndCloseComment();
  testEditsPastLinesAskedFor();
  testEditSequences();
  testErrors();
  testCacheBound();
  testSpeed();
  std::println("editing_source_lines_edits_test: ok");
  return 0;
}
