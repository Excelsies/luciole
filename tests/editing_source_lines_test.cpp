#include "editing/source_lines.h"
#include "formats/file_kind.h"
#include "formats/styled_text.h"
#include "test_check.h"

#include <cstddef>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
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

  void testCodePointRule() {
    const std::vector<std::string> lines{
        "héllo wörld ✓ \U0001F600 // cömment",
        "\tindent\tand tab",
        "a\x01"
        "b\x7F c\x1B[0m",
        "",
        std::string(10000, 'x') + "é",
    };
    for (const auto* name : {"a.txt", "a.cpp", "a.py", "a.json", "a.md", "a.diff"}) {
      auto styles = stylesOf(name, lines);
      TEST_CHECK(styles.lineCount() == lines.size());
      for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto text = luciole::formats::joinedText(lineOf(styles, i));
        TEST_CHECK(codePoints(text) == codePoints(lines[i]));
      }
      TEST_CHECK(luciole::formats::joinedText(lineOf(styles, 1)) == "\tindent\tand tab");
      TEST_CHECK(luciole::formats::joinedText(lineOf(styles, 2)) == "a␁b␡ c␛[0m");
      TEST_CHECK(luciole::formats::joinedText(lineOf(styles, 4)) == lines[4]);
    }
  }

  struct Expect {
    std::size_t line = 0;
    std::size_t column = 0;
    StyleRole role = StyleRole::Plain;
  };

  void checkRoles(std::string_view name, const std::vector<std::string>& lines, const std::vector<Expect>& expects) {
    auto styles = stylesOf(name, lines);
    for (const auto& expect : expects) {
      if (roleAt(lineOf(styles, expect.line), expect.column) != expect.role) {
        std::println(stderr, "{}: line {} column {}", name, expect.line, expect.column);
      }
      TEST_CHECK(roleAt(lineOf(styles, expect.line), expect.column) == expect.role);
    }
  }

  void testCLikeBlockComment() {
    checkRoles(
        "a.cpp", {"int a = 1; /* start", "still comment", "end */ return 0;"},
        {{0, 0, StyleRole::Keyword},
         {0, 8, StyleRole::Number},
         {0, 11, StyleRole::Comment},
         {1, 0, StyleRole::Comment},
         {1, 12, StyleRole::Comment},
         {2, 5, StyleRole::Comment},
         {2, 7, StyleRole::Keyword},
         {2, 14, StyleRole::Number}}
    );
  }

  void testPythonTripleQuotes() {
    checkRoles(
        "a.py", {R"(x = """doc)", "inside # not a comment", R"(end""" # real)"},
        {{0, 4, StyleRole::String},
         {1, 0, StyleRole::String},
         {1, 7, StyleRole::String},
         {2, 5, StyleRole::String},
         {2, 7, StyleRole::Comment}}
    );
  }

  void testKeyValueFamilies() {
    checkRoles(
        "a.yaml", {"key: value # note", "- item: 3", "  on: true"},
        {{0, 0, StyleRole::Key},
         {0, 3, StyleRole::Punctuation},
         {0, 11, StyleRole::Comment},
         {1, 0, StyleRole::Punctuation},
         {1, 2, StyleRole::Key},
         {1, 8, StyleRole::Number},
         {2, 2, StyleRole::Key},
         {2, 6, StyleRole::Literal}}
    );
    checkRoles(
        "a.toml", {"[server]", R"(name = """multi)", "line = 3", R"(end""")", "port = 80"},
        {{0, 0, StyleRole::Tag},
         {1, 0, StyleRole::Key},
         {1, 5, StyleRole::Punctuation},
         {1, 7, StyleRole::String},
         {2, 0, StyleRole::String},
         {2, 7, StyleRole::String},
         {3, 0, StyleRole::String},
         {4, 0, StyleRole::Key},
         {4, 7, StyleRole::Number}}
    );
    checkRoles(
        "a.ini", {"; note", "[section]", "key=value"},
        {{0, 0, StyleRole::Comment}, {1, 0, StyleRole::Tag}, {2, 0, StyleRole::Key}, {2, 3, StyleRole::Punctuation}}
    );
  }

  void testXmlCarry() {
    checkRoles(
        "a.xml", {"<a href=\"x\"", "   id='y'>text<![CDATA[raw", "still <b> raw]]><c/>"},
        {{0, 0, StyleRole::Punctuation},
         {0, 1, StyleRole::Tag},
         {0, 3, StyleRole::Attribute},
         {0, 8, StyleRole::String},
         {1, 3, StyleRole::Attribute},
         {1, 6, StyleRole::String},
         {1, 9, StyleRole::Punctuation},
         {1, 10, StyleRole::Plain},
         {2, 6, StyleRole::Plain},
         {2, 7, StyleRole::Plain},
         {2, 16, StyleRole::Punctuation},
         {2, 17, StyleRole::Tag}}
    );
  }

  void testDiffHunkCarry() {
    checkRoles(
        "a.diff", {"--- a/x", "+++ b/x", "@@ -1,2 +1,2 @@", "--- old", " same", "+++ new", "--- c"},
        {{0, 0, StyleRole::DiffHeader},
         {1, 0, StyleRole::DiffHeader},
         {2, 0, StyleRole::Hunk},
         {3, 0, StyleRole::Removal},
         {4, 1, StyleRole::Plain},
         {5, 0, StyleRole::Addition},
         {6, 0, StyleRole::DiffHeader}}
    );
  }

  void testJsonAsItStands() {
    const std::vector<std::string> lines{"{", R"(  "key": [1, true, null, "s"],)", R"(  "bad": nope, "open)", "}"};
    checkRoles(
        "a.json", lines,
        {{0, 0, StyleRole::Punctuation},
         {1, 2, StyleRole::Key},
         {1, 7, StyleRole::Punctuation},
         {1, 9, StyleRole::Punctuation},
         {1, 10, StyleRole::Number},
         {1, 13, StyleRole::Literal},
         {1, 19, StyleRole::Literal},
         {1, 25, StyleRole::String},
         {2, 2, StyleRole::Key},
         {2, 9, StyleRole::Literal},
         {2, 15, StyleRole::String},
         {3, 0, StyleRole::Punctuation}}
    );
    auto styles = stylesOf("a.json", lines);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      TEST_CHECK(luciole::formats::joinedText(lineOf(styles, i)) == lines[i]);
    }
  }

  void testPlainText() {
    checkRoles(
        "a.txt", {"int /* x", "\"y"}, {{0, 0, StyleRole::Plain}, {0, 4, StyleRole::Plain}, {1, 0, StyleRole::Plain}}
    );
  }

  // Code lines match the preview's own lines for every kind it colors line by line.
  void testMatchesPreviewLines() {
    const std::vector<std::pair<std::string_view, std::vector<std::string>>> samples{
        {"a.cpp", {"#include <x>", "int f() { /* a", "b */ return 0x10; } // c", R"(auto s = "q\"";)"}},
        {"a.py", {"def f():", "    '''doc", "    more'''", "    return None  # done"}},
        {"a.yaml", {"a: [1, 2]", "b: |", "  text", "c: 2024-01-02"}},
        {"a.toml", {"[a.b]", "k = '''x", "y'''", "n = inf"}},
        {"a.ini", {"[s]", "a = 1 ; c", "b: yes"}},
        {"a.xml", {"<?xml version=\"1.0\"?>", "<!-- c", "-->", "<a b='1'", "/>"}},
        {"a.diff", {"diff --git a b", "@@ -1 +1,2 @@", "-x", "+y", "+z", "\\ No newline"}},
        {"a.css", {"a { color: red; }", "b { background: #00ff00 /* x", "*/ }"}},
        {"a.txt", {"plain", "\ttext"}},
    };
    for (const auto& [name, lines] : samples) {
      std::string text;
      for (const auto& line : lines) {
        text += line + "\n";
      }
      const auto document = luciole::formats::styledText(text, fileKind(name));
      TEST_CHECK(document.blocks.size() == 1 && document.blocks[0].lines.size() == lines.size());
      auto styles = stylesOf(name, lines);
      for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = lineOf(styles, i);
        TEST_CHECK(line.runs == document.blocks[0].lines[i].runs);
        TEST_CHECK(line.swatches == document.blocks[0].lines[i].swatches);
      }
    }
  }

  using luciole::formats::ColorSwatch;
  using luciole::formats::Rgba;

  void testSwatchColumns() {
    const std::vector<std::string> cssLines{
        "/* é\U0001F600 */ b { color: rgb(0, 0, 255) }", "/* ü */ a { color: red; }"
    };
    auto css = stylesOf("a.css", cssLines);
    TEST_CHECK((lineOf(css, 0).swatches == std::vector<ColorSwatch>{{.color = Rgba{0, 0, 255, 255}, .column = 20}}));
    TEST_CHECK((lineOf(css, 1).swatches == std::vector<ColorSwatch>{{.color = Rgba{255, 0, 0, 255}, .column = 19}}));
    // Color names count only in CSS and SCSS.
    const std::vector<std::string> yamlLines{"ñame: '#00ff00' red"};
    auto yaml = stylesOf("a.yaml", yamlLines);
    TEST_CHECK((lineOf(yaml, 0).swatches == std::vector<ColorSwatch>{{.color = Rgba{0, 255, 0, 255}, .column = 7}}));
    const std::vector<std::string> pythonLines{"x = \"é #ff0000\""};
    auto python = stylesOf("a.py", pythonLines);
    TEST_CHECK((lineOf(python, 0).swatches == std::vector<ColorSwatch>{{.color = Rgba{255, 0, 0, 255}, .column = 7}}));
  }

} // namespace

int main() {
  testCodePointRule();
  testCLikeBlockComment();
  testPythonTripleQuotes();
  testKeyValueFamilies();
  testXmlCarry();
  testDiffHunkCarry();
  testJsonAsItStands();
  testPlainText();
  testMatchesPreviewLines();
  testSwatchColumns();
  std::println("editing_source_lines_test: ok");
  return 0;
}
