#include "formats/styled_text.h"
#include "test_check.h"

#include <cstddef>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  std::string repeat(std::string_view piece, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += piece;
    }
    return out;
  }

  std::size_t codePoints(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
      count += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return count;
  }

  const StyledLine& onlyLine(const StyledDocument& document) {
    TEST_CHECK(document.blocks.size() == 1);
    TEST_CHECK(document.blocks[0].lines.size() == 1);
    return document.blocks[0].lines[0];
  }

  // The role of the first run whose text is `text`.
  std::optional<StyleRole> roleOf(const StyledLine& line, std::string_view text) {
    for (const auto& run : line.runs) {
      if (run.text == text) {
        return run.role;
      }
    }
    return std::nullopt;
  }

  void testCodeRoles() {
    const auto document = styledText("int main() { return 0; } // done", fileKind("a.c"));
    TEST_CHECK(document.blocks.size() == 1);
    TEST_CHECK(document.blocks[0].kind == BlockKind::Code);
    TEST_CHECK(!document.blocks[0].framed);
    const auto& line = onlyLine(document);
    TEST_CHECK(roleOf(line, "int") == StyleRole::Keyword);
    TEST_CHECK(roleOf(line, "return") == StyleRole::Keyword);
    TEST_CHECK(roleOf(line, "0") == StyleRole::Number);
    TEST_CHECK(roleOf(line, "// done") == StyleRole::Comment);
    TEST_CHECK(joinedText(line) == "int main() { return 0; } // done");
    TEST_CHECK(!document.truncated());
  }

  void testConfigRoles() {
    const auto toml = styledText("[server]\nport = 8080 # main\nname = \"x\"\n", fileKind("a.toml"));
    TEST_CHECK(toml.blocks.size() == 1);
    const auto& lines = toml.blocks[0].lines;
    TEST_CHECK(lines.size() == 3);
    TEST_CHECK(roleOf(lines[1], "port") == StyleRole::Key);
    TEST_CHECK(roleOf(lines[1], "8080") == StyleRole::Number);
    TEST_CHECK(roleOf(lines[1], "# main") == StyleRole::Comment);
    TEST_CHECK(roleOf(lines[2], "\"x\"") == StyleRole::String);

    const auto diff = styledText("--- a\n+++ b\n@@ -1 +1 @@\n-old\n+new\n", fileKind("a.diff"));
    const auto& diffLines = diff.blocks[0].lines;
    TEST_CHECK(diffLines.size() == 5);
    TEST_CHECK(roleOf(diffLines[3], "-old") == StyleRole::Removal);
    TEST_CHECK(roleOf(diffLines[4], "+new") == StyleRole::Addition);
    TEST_CHECK(diffLines[2].runs[0].role == StyleRole::Hunk);
    TEST_CHECK(diffLines[0].runs[0].role == StyleRole::DiffHeader);
  }

  void testJsonIsIndented() {
    const auto document = styledText(R"({"a":1,"b":[true,"s"]})", fileKind("a.json"));
    TEST_CHECK(!document.problem);
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == 7);
    TEST_CHECK(joinedText(lines[0]) == "{");
    TEST_CHECK(joinedText(lines[1]) == R"(  "a": 1,)");
    TEST_CHECK(joinedText(lines[3]) == "    true,");
    TEST_CHECK(joinedText(lines[6]) == "}");
    TEST_CHECK(roleOf(lines[1], "\"a\"") == StyleRole::Key);
    TEST_CHECK(roleOf(lines[1], "1") == StyleRole::Number);
    TEST_CHECK(roleOf(lines[3], "true") == StyleRole::Literal);
    TEST_CHECK(roleOf(lines[4], "\"s\"") == StyleRole::String);
  }

  void testInvalidJsonIsRaw() {
    const auto document = styledText("{\"a\":\n1,,}", fileKind("a.json"));
    TEST_CHECK(document.problem.has_value());
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == 2);
    TEST_CHECK(joinedText(lines[0]) == "{\"a\":");
    TEST_CHECK(joinedText(lines[1]) == "1,,}");
  }

  void testCutJsonIsPrinted() {
    // A minified document cut at the byte limit is printed up to the cut, and its last line continues.
    const std::string minified = "[" + repeat(R"({"k":1},)", 6) + R"({"k":"long text"}])";
    const auto document = styledText(minified, fileKind("a.json"), {.bytes = 59});
    TEST_CHECK(document.bytesCapped && document.truncated());
    TEST_CHECK(!document.problem && !document.widthCapped && !document.linesCapped);
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == 21);
    TEST_CHECK(joinedText(lines[0]) == "[");
    TEST_CHECK(joinedText(lines[1]) == "  {");
    TEST_CHECK(joinedText(lines[2]) == "    \"k\": 1");
    TEST_CHECK(joinedText(lines[3]) == "  },");
    TEST_CHECK(roleOf(lines[2], "\"k\"") == StyleRole::Key);
    TEST_CHECK(roleOf(lines[2], "1") == StyleRole::Number);
    TEST_CHECK(!lines[3].continues && !lines[19].continues);
    TEST_CHECK(joinedText(lines[20]) == "    \"k\": \"long");
    TEST_CHECK(roleOf(lines[20], "\"long") == StyleRole::String);
    TEST_CHECK(lines[20].continues);

    // Indented text cut at the limit is printed the same way.
    const std::string indented = "[\n" + repeat("  {\"k\": 1},\n", 40) + "  {\"k\": 2}\n]";
    const auto cut = styledText(indented, fileKind("a.json"), {.bytes = 104});
    TEST_CHECK(cut.bytesCapped && !cut.problem);
    const auto& cutLines = cut.blocks[0].lines;
    TEST_CHECK(cutLines.size() == 27);
    TEST_CHECK(joinedText(cutLines[2]) == "    \"k\": 1");
    TEST_CHECK(joinedText(cutLines[26]) == "    \"k\"" && cutLines[26].continues);

    // A document complete at the cut has nothing after its last line.
    const auto complete = styledText("{\"a\":1}" + repeat(" ", 100), fileKind("a.json"), {.bytes = 7});
    TEST_CHECK(complete.bytesCapped && complete.blocks[0].lines.size() == 3);
    TEST_CHECK(!complete.blocks[0].lines[2].continues);

    // Text that is not the start of one JSON value stays line by line as it stands, with no problem reported.
    const auto values = styledText(repeat("{\"k\":1},", 40), fileKind("a.json"), {.bytes = 50});
    TEST_CHECK(values.bytesCapped && !values.problem);
    TEST_CHECK(joinedText(onlyLine(values)) == repeat("{\"k\":1},", 6) + "{\"");
    const auto broken = styledText("[1,,\n" + repeat("2,\n", 40), fileKind("a.json"), {.bytes = 20});
    TEST_CHECK(broken.bytesCapped && !broken.problem);
    TEST_CHECK(joinedText(broken.blocks[0].lines[0]) == "[1,,");
  }

  // A minified file past the read limit fills the line limit.
  void testLargeMinifiedJson() {
    const std::string large = "[" + repeat(R"({"name":"value","n":12345},)", 12000) + "1]";
    TEST_CHECK(large.size() > kStyledTextBytes);
    const auto document = styledText(large, fileKind("a.json"));
    TEST_CHECK(document.bytesCapped && document.linesCapped && !document.problem);
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == kStyledTextLines);
    TEST_CHECK(joinedText(lines[2]) == "    \"name\": \"value\",");
    TEST_CHECK(!lines.back().continues);
  }

  void testPlainAndUnknownKinds() {
    const auto plain = styledText("one\r\ntwo\rthree\n", fileKind("a.txt"));
    TEST_CHECK(plain.blocks.size() == 1);
    TEST_CHECK(plain.blocks[0].kind == BlockKind::Prose);
    TEST_CHECK(plain.blocks[0].lines.size() == 3);
    TEST_CHECK(joinedText(plain.blocks[0].lines[2]) == "three");
    TEST_CHECK(plain.blocks[0].lines[0].runs[0].role == StyleRole::Plain);

    const auto unknown = styledText("int x", FileKind{});
    TEST_CHECK(onlyLine(unknown).runs.size() == 1);
    TEST_CHECK(onlyLine(unknown).runs[0].role == StyleRole::Plain);

    const auto empty = styledText("", fileKind("a.c"));
    TEST_CHECK(empty.blocks.size() == 1);
    TEST_CHECK(empty.blocks[0].lines.size() == 1);
    TEST_CHECK(joinedText(empty.blocks[0].lines[0]).empty());
  }

  void testProseAndCodeBlocks() {
    // Prose kinds are one prose block of the file's own lines, blank lines kept, spacing kept, never cut at columns.
    const auto prose = styledText("First  line\n\n  indented\n", fileKind("notes.txt"), {.columns = 4});
    TEST_CHECK(prose.blocks.size() == 1);
    TEST_CHECK(prose.blocks[0].kind == BlockKind::Prose);
    TEST_CHECK(prose.blocks[0].lines.size() == 3);
    TEST_CHECK(joinedText(prose.blocks[0].lines[0]) == "First  line");
    TEST_CHECK(joinedText(prose.blocks[0].lines[1]).empty());
    TEST_CHECK(joinedText(prose.blocks[0].lines[2]) == "  indented");
    TEST_CHECK(!prose.widthCapped);
    TEST_CHECK(!prose.blocks[0].lines[0].continues);

    // A line past the code column limit but within a segment is whole.
    const auto wide = styledText(repeat("word ", 200), fileKind("README"));
    TEST_CHECK(wide.blocks[0].kind == BlockKind::Prose);
    TEST_CHECK(joinedText(onlyLine(wide)).size() == 1000);
    TEST_CHECK(!wide.widthCapped && !wide.truncated());

    // Everything else that is plain stays code: logs, rst, ignore lists, unknown names.
    for (const auto* name : {"build.log", "a.rst", ".gitignore", "yarn.lock"}) {
      TEST_CHECK(styledText("a\nb", fileKind(name)).blocks[0].kind == BlockKind::Code);
    }
    TEST_CHECK(styledText("a\nb", FileKind{}).blocks[0].kind == BlockKind::Code);
    TEST_CHECK(styledText("a\nb", fileKind("a.c")).blocks[0].kind == BlockKind::Code);
  }

  std::string allText(const StyledBlock& block) {
    std::string out;
    for (const auto& line : block.lines) {
      out += joinedText(line);
    }
    return out;
  }

  // A prose line longer than a segment is split after a space, so that no layout holds an unbounded paragraph.
  void testLongProseLinesAreSegmented() {
    const auto words = repeat("word ", 2500); // 12,500 code points
    const auto document = styledText(words, fileKind("a.txt"));
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == 13);
    TEST_CHECK(allText(document.blocks[0]) == words);
    for (const auto& line : lines) {
      const auto text = joinedText(line);
      TEST_CHECK(codePoints(text) <= kStyledProseSegment);
      TEST_CHECK(text.ends_with(' '));
      TEST_CHECK(!line.continues);
    }
    TEST_CHECK(codePoints(joinedText(lines[0])) == 1020); // 204 words and their spaces
    TEST_CHECK(!document.truncated());

    // A word longer than a segment breaks inside itself, on code point boundaries.
    const auto word = repeat("é", 10000);
    const auto broken = styledText(word, fileKind("a.txt"));
    TEST_CHECK(broken.blocks[0].lines.size() == 10);
    TEST_CHECK(codePoints(joinedText(broken.blocks[0].lines[0])) == kStyledProseSegment);
    TEST_CHECK(codePoints(joinedText(broken.blocks[0].lines[1])) == kStyledProseSegment);
    TEST_CHECK(codePoints(joinedText(broken.blocks[0].lines[9])) == 10000 - (9 * kStyledProseSegment));
    TEST_CHECK(allText(broken.blocks[0]) == word);

    // Each segment is a line of the limit, and each next line of the file follows its segments.
    const auto several = styledText(repeat("word ", 800) + "\nlast", fileKind("a.txt"), {.lines = 5});
    TEST_CHECK(several.blocks[0].lines.size() == 5);
    TEST_CHECK(joinedText(several.blocks[0].lines[4]) == "last");
    const auto capped = styledText(words, fileKind("a.txt"), {.lines = 2});
    TEST_CHECK(capped.linesCapped && capped.blocks[0].lines.size() == 2);

    // A megabyte on one line is read to the byte limit and becomes a bounded number of segments.
    const auto huge = styledText(repeat("lorem ipsum dolor sit amet, ", 40000), fileKind("a.txt"));
    TEST_CHECK(huge.bytesCapped);
    TEST_CHECK(huge.blocks[0].lines.size() <= (kStyledTextBytes / (kStyledProseSegment - 64)) + 2);
    TEST_CHECK(huge.blocks[0].lines.size() >= kStyledTextBytes / kStyledProseSegment);

    // Code lines are cut at the column limit instead.
    const auto code = styledText(words, fileKind("a.log"));
    TEST_CHECK(code.blocks[0].lines.size() == 1);
    TEST_CHECK(code.blocks[0].lines[0].continues);
  }

  // A Markdown paragraph of many source lines, the longest the parser keeps, is split like plain prose, in the
  // styles it had.
  void testLongMarkdownLinesAreSegmented() {
    const auto row = std::string_view{"word word word word word word word word word\n"}; // 45 code points
    const auto paragraph = repeat(row, 1000);
    const auto trimmed = paragraph.substr(0, paragraph.size() - 1);
    const auto heading = "# " + repeat("word ", 300) + "\n\n";
    const auto document = styledText(heading + "plain *" + trimmed + "* end", fileKind("a.md"));
    TEST_CHECK(!document.bytesCapped && !document.parserCapped);
    std::size_t emphasized = 0;
    std::size_t total = 0;
    for (const auto& block : document.blocks) {
      for (const auto& line : block.lines) {
        TEST_CHECK(codePoints(joinedText(line)) <= kStyledProseSegment);
        ++total;
        for (const auto& run : line.runs) {
          emphasized += run.italic ? codePoints(run.text) : 0;
        }
      }
    }
    TEST_CHECK(total >= 2 + (45000 / kStyledProseSegment));
    TEST_CHECK(emphasized >= 44000);
    TEST_CHECK(document.blocks.size() == 2 && document.blocks[0].lines.size() == 2);
    TEST_CHECK(!document.linesCapped);
  }

  // A forced split inside a word keeps combining marks and joined characters with their base.
  void testSegmentsKeepClustersTogether() {
    const auto marked = "x" + repeat("e\xCC\x81", 3000); // the 1,025th code point is a combining acute
    const auto document = styledText(marked, fileKind("a.txt"));
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() >= 5);
    TEST_CHECK(allText(document.blocks[0]) == marked);
    for (const auto& line : lines) {
      TEST_CHECK(!joinedText(line).starts_with("\xCC\x81"));
    }
    const auto joined = repeat("a", kStyledProseSegment - 1) + "\xE2\x80\x8D" + "b" + repeat("c", 10);
    const auto zwj = styledText(joined, fileKind("a.txt"));
    TEST_CHECK(zwj.blocks[0].lines.size() == 2);
    TEST_CHECK(joinedText(zwj.blocks[0].lines[0])
                   .ends_with(
                       "\xE2\x80\x8D"
                       "b"
                   ));
    TEST_CHECK(allText(zwj.blocks[0]) == joined);
  }

  void testByteLimit() {
    const auto text = repeat("abcdefghi\n", 100);
    const auto document = styledText(text, fileKind("a.txt"), {.bytes = 25});
    TEST_CHECK(document.bytesCapped);
    TEST_CHECK(document.truncated());
    const auto& lines = document.blocks[0].lines;
    TEST_CHECK(lines.size() == 3);
    TEST_CHECK(joinedText(lines[2]) == "abcde");
    TEST_CHECK(!styledText(text, fileKind("a.txt"), {.bytes = text.size()}).bytesCapped);

    // A cut inside a UTF-8 sequence backs off to its start.
    const auto accents = styledText("éé", fileKind("a.txt"), {.bytes = 3});
    TEST_CHECK(joinedText(onlyLine(accents)) == "é");
  }

  void testLineLimit() {
    const auto text = repeat("x\n", 50);
    const auto document = styledText(text, fileKind("a.txt"), {.lines = 10});
    TEST_CHECK(document.linesCapped);
    TEST_CHECK(document.blocks[0].lines.size() == 10);
    const auto exact = styledText(repeat("x\n", 10), fileKind("a.txt"), {.lines = 10});
    TEST_CHECK(!exact.linesCapped);
    TEST_CHECK(exact.blocks[0].lines.size() == 10);

    const auto json = styledText("[1,2,3,4,5,6,7,8]", fileKind("a.json"), {.lines = 4});
    TEST_CHECK(json.linesCapped);
    TEST_CHECK(json.blocks[0].lines.size() == 4);

    const auto none = styledText("a\nb", fileKind("a.txt"), {.lines = 0});
    TEST_CHECK(none.linesCapped);
    TEST_CHECK(none.blocks.empty());
  }

  void testColumnLimit() {
    const auto text = "int " + repeat("é", 30);
    const auto document = styledText(text, fileKind("a.c"), {.columns = 10});
    const auto& line = onlyLine(document);
    TEST_CHECK(document.widthCapped);
    TEST_CHECK(line.continues);
    TEST_CHECK(codePoints(joinedText(line)) == 10);
    TEST_CHECK(joinedText(line) == "int " + repeat("é", 6));
    TEST_CHECK(roleOf(line, "int") == StyleRole::Keyword);

    const auto fits = styledText("0123456789", fileKind("a.txt"), {.columns = 10});
    TEST_CHECK(!fits.widthCapped);
    TEST_CHECK(!onlyLine(fits).continues);

    const auto defaults = styledText(repeat("a", kStyledLineColumns + 5), fileKind("a.log"));
    TEST_CHECK(codePoints(joinedText(onlyLine(defaults))) == kStyledLineColumns);
    TEST_CHECK(onlyLine(defaults).continues);
  }

  void testHostileBytes() {
    const std::string text{"a\0b\x01\x7F\xFF\xC3(\t\xE2\x82", 11};
    const auto document = styledText(text, fileKind("a.c"));
    const auto joined = joinedText(onlyLine(document));
    TEST_CHECK(joined == "a␀b␁␡��(\t��");
    TEST_CHECK(styledText("\xEF\xBB\xBFhi", fileKind("a.txt")).blocks[0].lines[0].runs[0].text == "hi");
    const auto json = styledText(std::string{"{\"a\0\":1}", 8}, fileKind("a.json"));
    TEST_CHECK(!json.problem);
    TEST_CHECK(joinedText(json.blocks[0].lines[1]) == "  \"a␀\": 1");

    // Four-byte sequences pass; a surrogate or an overlong form does not.
    TEST_CHECK(joinedText(onlyLine(styledText("x \xF0\x9F\x98\x80 y", fileKind("a.txt")))) == "x \xF0\x9F\x98\x80 y");
    TEST_CHECK(joinedText(onlyLine(styledText("\xED\xA0\x80", fileKind("a.txt")))) == "\uFFFD\uFFFD\uFFFD");
    TEST_CHECK(joinedText(onlyLine(styledText("\xC0\xAF", fileKind("a.txt")))) == "\uFFFD\uFFFD");
  }

  void testSwatches() {
    const auto css = styledText("a { color: #ff0000; background: rgba(0, 0, 255, 0.5); }\n", fileKind("a.css"));
    const auto& line = css.blocks[0].lines[0];
    TEST_CHECK(line.swatches.size() == 2);
    TEST_CHECK((line.swatches[0] == ColorSwatch{.color = {.r = 255, .g = 0, .b = 0, .a = 255}, .column = 11}));
    TEST_CHECK(line.swatches[1].column == 32);
    TEST_CHECK(line.swatches[1].color.b == 255);
    TEST_CHECK(line.swatches[1].color.a == 128);

    // Columns count code points, not bytes.
    const auto wide = styledText("é: \"#00ff00\"", fileKind("a.yaml"));
    TEST_CHECK(onlyLine(wide).swatches.size() == 1);
    TEST_CHECK(onlyLine(wide).swatches[0].column == 4);

    // Names only in CSS and SCSS.
    TEST_CHECK(onlyLine(styledText("a { color: red }", fileKind("a.css"))).swatches.size() == 1);
    TEST_CHECK(onlyLine(styledText("color: red", fileKind("a.yaml"))).swatches.empty());
    TEST_CHECK(onlyLine(styledText("#ff0000", fileKind("a.txt"))).swatches.empty());

    // A code cut by the column limit has no swatch.
    const auto cut = styledText("a: '#ff0000'", fileKind("a.yaml"), {.columns = 6});
    TEST_CHECK(onlyLine(cut).swatches.empty());
    const auto whole = styledText("a: '#ff0000'", fileKind("a.yaml"), {.columns = 11});
    TEST_CHECK(onlyLine(whole).swatches.size() == 1);

    // JSON swatches sit in the printed text.
    const auto json = styledText(R"({"fg":"#0000ff"})", fileKind("a.json"));
    TEST_CHECK(json.blocks[0].lines[1].swatches.size() == 1);
    TEST_CHECK(json.blocks[0].lines[1].swatches[0].column == 9);
  }

  void testSwatchCapPerFile() {
    const auto text = repeat("a: #fff #fff #fff #fff #fff #fff #fff #fff #fff #fff\n", 500);
    const auto document = styledText(text, fileKind("a.yaml"));
    std::size_t count = 0;
    for (const auto& line : document.blocks[0].lines) {
      count += line.swatches.size();
    }
    TEST_CHECK(count == kMaxColorsPerFile);
  }

  StyledDocument markdown(std::string_view text, const StyledTextLimits& limits = {}) {
    return styledText(text, fileKind("a.md"), limits);
  }

  void testMarkdownHeadingsAndInline() {
    const auto document = markdown("# Title *it*\n\nBody **bold** and *it* and `x` and [link](http://a) ![alt](i.png)");
    TEST_CHECK(document.blocks.size() == 2);
    const auto& heading = document.blocks[0];
    TEST_CHECK(heading.kind == BlockKind::Prose);
    TEST_CHECK(heading.heading == 1);
    TEST_CHECK(heading.lines[0].runs[0].role == StyleRole::Heading);
    TEST_CHECK(heading.lines[0].runs[1].text == "it");
    TEST_CHECK(heading.lines[0].runs[1].role == StyleRole::Heading);
    TEST_CHECK(heading.lines[0].runs[1].italic);

    const auto& body = document.blocks[1].lines[0];
    TEST_CHECK(document.blocks[1].heading == 0);
    TEST_CHECK(joinedText(body) == "Body bold and it and x and link alt");
    TEST_CHECK(roleOf(body, "Body ") == StyleRole::Plain);
    TEST_CHECK(roleOf(body, "bold") == StyleRole::Strong);
    TEST_CHECK(body.runs[1].bold);
    TEST_CHECK(roleOf(body, "it") == StyleRole::Emphasis);
    TEST_CHECK(roleOf(body, "x") == StyleRole::InlineCode);
    TEST_CHECK(roleOf(body, "link") == StyleRole::Link);
    TEST_CHECK(roleOf(body, "alt") == StyleRole::Link);
    TEST_CHECK(!document.truncated());
  }

  void testMarkdownBreaksAndParagraphs() {
    const auto document = markdown("one\ntwo  \nthree\n\nfour");
    TEST_CHECK(document.blocks.size() == 2);
    TEST_CHECK(document.blocks[0].lines.size() == 2);
    TEST_CHECK(joinedText(document.blocks[0].lines[0]) == "one two");
    TEST_CHECK(joinedText(document.blocks[0].lines[1]) == "three");
  }

  void testMarkdownLists() {
    const auto document = markdown("- a\n- b\n  - c\n\n3. x\n4. y\n");
    TEST_CHECK(document.blocks.size() == 5);
    TEST_CHECK(document.blocks[0].marker == "•");
    TEST_CHECK(document.blocks[0].indent == 1);
    TEST_CHECK(joinedText(document.blocks[0].lines[0]) == "a");
    TEST_CHECK(document.blocks[1].marker == "•");
    TEST_CHECK(document.blocks[2].indent == 2);
    TEST_CHECK(joinedText(document.blocks[2].lines[0]) == "c");
    TEST_CHECK(document.blocks[3].marker == "3.");
    TEST_CHECK(document.blocks[4].marker == "4.");
    TEST_CHECK(document.blocks[4].indent == 1);

    const auto empty = markdown("-\n- b");
    TEST_CHECK(empty.blocks.size() == 2);
    TEST_CHECK(empty.blocks[0].marker == "•");
    TEST_CHECK(joinedText(empty.blocks[0].lines[0]).empty());

    // A list that opens an item shares the item's marker line.
    const auto nested = markdown("- - a\n  - b");
    TEST_CHECK(nested.blocks.size() == 2);
    TEST_CHECK(nested.blocks[0].marker == "• •");
    TEST_CHECK(nested.blocks[0].indent == 2);
    TEST_CHECK(nested.blocks[1].marker == "•");
  }

  void testMarkdownQuotes() {
    const auto document = markdown("> said *so*\n>\n> > deeper");
    TEST_CHECK(document.blocks.size() == 2);
    TEST_CHECK(document.blocks[0].quoteDepth == 1);
    TEST_CHECK(roleOf(document.blocks[0].lines[0], "said ") == StyleRole::Quote);
    TEST_CHECK(roleOf(document.blocks[0].lines[0], "so") == StyleRole::Emphasis);
    TEST_CHECK(document.blocks[1].quoteDepth == 2);
  }

  void testMarkdownCodeBlocks() {
    const auto document = markdown("```css\na { color: #00ff00; }\n```\n\n```\nint x\n```\n\n    indented\n");
    TEST_CHECK(document.blocks.size() == 3);
    const auto& css = document.blocks[0];
    TEST_CHECK(css.kind == BlockKind::Code);
    TEST_CHECK(css.framed);
    TEST_CHECK(css.lines.size() == 1);
    TEST_CHECK(css.lines[0].swatches.size() == 1);
    TEST_CHECK(css.lines[0].swatches[0].column == 11);
    TEST_CHECK(roleOf(document.blocks[1].lines[0], "int x") == StyleRole::Plain);
    TEST_CHECK(document.blocks[2].kind == BlockKind::Code);
    TEST_CHECK(joinedText(document.blocks[2].lines[0]) == "indented");

    const auto c = markdown("```c\nreturn 1;\n```");
    TEST_CHECK(roleOf(c.blocks[0].lines[0], "return") == StyleRole::Keyword);
  }

  void testMarkdownTablesAndRules() {
    const auto document = markdown("| a | b | c |\n|:--|:-:|--:|\n| 1 | **2** | 3 |\n\n---\n");
    TEST_CHECK(document.blocks.size() == 2);
    const auto& table = document.blocks[0];
    TEST_CHECK(table.kind == BlockKind::Table);
    TEST_CHECK(table.columns == 3);
    TEST_CHECK((table.alignments == std::vector{CellAlign::Start, CellAlign::Center, CellAlign::End}));
    TEST_CHECK(table.lines.size() == 6);
    TEST_CHECK(joinedText(table.lines[0]) == "a");
    TEST_CHECK(table.lines[0].runs[0].bold);
    TEST_CHECK(joinedText(table.lines[4]) == "2");
    TEST_CHECK(table.lines[4].runs[0].role == StyleRole::Strong);
    TEST_CHECK(!table.lines[3].runs[0].bold);
    TEST_CHECK(document.blocks[1].kind == BlockKind::Rule);
    TEST_CHECK(document.blocks[1].lines.empty());
  }

  void testMarkdownLimits() {
    const auto many = markdown(repeat("para\n\n", 30), {.lines = 5});
    TEST_CHECK(many.linesCapped);
    TEST_CHECK(many.blocks.size() == 5);

    // A table keeps whole rows.
    const auto table = markdown("| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n", {.lines = 5});
    TEST_CHECK(table.linesCapped);
    TEST_CHECK(table.blocks[0].lines.size() == 4);

    // Prose wraps, so the column limit cuts only code lines and table cells.
    const auto wide = markdown(repeat("word ", 100) + "\n\n```\n" + repeat("x", 20) + "\n```\n", {.columns = 12});
    TEST_CHECK(wide.blocks.size() == 2);
    TEST_CHECK(codePoints(joinedText(wide.blocks[0].lines[0])) == 499);
    TEST_CHECK(!wide.blocks[0].lines[0].continues);
    TEST_CHECK(wide.blocks[1].lines[0].continues);
    TEST_CHECK(wide.widthCapped);
    const auto cell = markdown("| " + repeat("y", 20) + " |\n|---|\n", {.columns = 12});
    TEST_CHECK(codePoints(joinedText(cell.blocks[0].lines[0])) == 12);
    TEST_CHECK(cell.widthCapped);
    const auto prose = markdown(repeat("word word word word word word word word word word\n", 100));
    TEST_CHECK(codePoints(allText(prose.blocks[0])) == 4999);
    TEST_CHECK(prose.blocks[0].lines.size() > 4);
    TEST_CHECK(!prose.truncated());

    const auto huge = markdown(repeat("word ", 50000), {.bytes = 300000});
    TEST_CHECK(huge.parserCapped);
    TEST_CHECK(!huge.bytesCapped);
    TEST_CHECK(huge.truncated());
  }
} // namespace

int main() {
  testCodeRoles();
  testConfigRoles();
  testJsonIsIndented();
  testInvalidJsonIsRaw();
  testCutJsonIsPrinted();
  testLargeMinifiedJson();
  testPlainAndUnknownKinds();
  testProseAndCodeBlocks();
  testLongProseLinesAreSegmented();
  testLongMarkdownLinesAreSegmented();
  testSegmentsKeepClustersTogether();
  testByteLimit();
  testLineLimit();
  testColumnLimit();
  testHostileBytes();
  testSwatches();
  testSwatchCapPerFile();
  testMarkdownHeadingsAndInline();
  testMarkdownBreaksAndParagraphs();
  testMarkdownLists();
  testMarkdownQuotes();
  testMarkdownCodeBlocks();
  testMarkdownTablesAndRules();
  testMarkdownLimits();
  std::println("formats styled text checks passed");
  return 0;
}
