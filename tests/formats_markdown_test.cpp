#include "formats/markdown.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats::markdown;

  std::string repeat(std::string_view unit, std::size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (std::size_t k = 0; k < count; ++k) {
      out += unit;
    }
    return out;
  }

  // Runs as compact text: "em(a)", "strong(b)", "code(c)", "link[href](t)", "image[src](alt)", "~" for a break.
  // Recursion is bounded by the parser's own nesting limits.
  std::string show(const Document& document, Range range) {
    std::string out;
    for (const Run& run : runs(document, range)) {
      const std::string_view value = text(document, run.text);
      switch (run.kind) {
      case RunKind::Text:
        out += value;
        break;
      case RunKind::Code:
        out += std::format("code({})", value);
        break;
      case RunKind::Break:
        out += '~';
        break;
      case RunKind::Link:
        out += std::format("link[{}]({})", value, show(document, run.children));
        break;
      case RunKind::Image:
        out += std::format("image[{}]({})", value, text(document, run.alt));
        break;
      case RunKind::Emphasis:
        out += std::format("em({})", show(document, run.children));
        break;
      case RunKind::Strong:
        out += std::format("strong({})", show(document, run.children));
        break;
      }
    }
    return out;
  }

  std::vector<Block> top(const Document& document) {
    const std::span<const Block> list = blocks(document, document.root);
    return {list.begin(), list.end()};
  }

  std::vector<BlockKind> kinds(const Document& document, Range range) {
    std::vector<BlockKind> out;
    for (const Block& block : blocks(document, range)) {
      out.push_back(block.kind);
    }
    return out;
  }

  // The inline runs of a source that must parse to exactly one paragraph.
  std::string para(std::string_view source) {
    const Document document = parse(source);
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 1);
    TEST_CHECK(list[0].kind == BlockKind::Paragraph);
    return show(document, list[0].runs);
  }

  void testHeadings() {
    const Document document =
        parse("# One\n## Two ##\n###### Six\n####### no\n\n#nospace\n\nTitle\n=====\n\nSub\n---\n");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 7);
    const std::vector<BlockKind> expected{BlockKind::Heading,   BlockKind::Heading,   BlockKind::Heading,
                                          BlockKind::Paragraph, BlockKind::Paragraph, BlockKind::Heading,
                                          BlockKind::Heading};
    TEST_CHECK(kinds(document, document.root) == expected);
    const std::vector<int> levels{list[0].level, list[1].level, list[2].level, list[5].level, list[6].level};
    TEST_CHECK((levels == std::vector<int>{1, 2, 6, 1, 2}));
    TEST_CHECK(list[3].level == 0);
    TEST_CHECK(list[4].level == 0);
    TEST_CHECK(show(document, list[1].runs) == "Two");
    TEST_CHECK(show(document, list[5].runs) == "Title");
    TEST_CHECK(show(document, list[6].runs) == "Sub");
  }

  void testParagraphs() {
    TEST_CHECK(para("a\nb") == "a b");
    TEST_CHECK(top(parse("a\n\nb")).size() == 2);
    TEST_CHECK(para("a  \nb") == "a~b");
    TEST_CHECK(para("a\\\nb") == "a~b");
  }

  void testEmphasis() {
    TEST_CHECK(para("*a* _b_ **c** __d__") == "em(a) em(b) strong(c) strong(d)");
    TEST_CHECK(para("**a *b* c**") == "strong(a em(b) c)");
    TEST_CHECK(para("***x***") == "em(strong(x))");
    TEST_CHECK(para("2 * 3 * 4") == "2 * 3 * 4");
    TEST_CHECK(para("snake_case_name") == "snake_case_name");
    TEST_CHECK(para("*open only") == "*open only");
    TEST_CHECK(para("a\\*b") == "a*b");
  }

  void testCodeSpans() {
    TEST_CHECK(para("use `a*b*` here") == "use code(a*b*) here");
    TEST_CHECK(para("``a ` b``") == "code(a ` b)");
    TEST_CHECK(para("`open") == "`open");
  }

  void testLinksAndImagesAreData() {
    TEST_CHECK(para("[text](http://x.y/z)") == "link[http://x.y/z](text)");
    TEST_CHECK(para("[**b**](u)") == "link[u](strong(b))");
    TEST_CHECK(para("![alt text](a.png)") == "image[a.png](alt text)");
    // The target is kept as data; nothing here makes it a live link.
    TEST_CHECK(
        para(
            "[js](java"
            "script:alert(1))"
        )
        == "link[java"
           "script:alert(1)](js)"
    );
    TEST_CHECK(para("[broken](no close") == "[broken](no close");
    TEST_CHECK(para("[ref] and [a][b]") == "[ref] and [a][b]");
  }

  void testRawHtmlIsText() {
    TEST_CHECK(para("<script>alert(1)</script>") == "<script>alert(1)</script>");
    TEST_CHECK(para("a <img src=x onerror=y> b") == "a <img src=x onerror=y> b");
    const Document document = parse("<div>\n<script>x</script>\n</div>");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 1);
    TEST_CHECK(show(document, list[0].runs).contains("<script>x</script>"));
  }

  void testFencedCode() {
    const Document document =
        parse("```js\nlet *a* = 1;\n# not a heading\n```\n~~~\nplain\n~~~\n````\nin ``` here\n````\n```\nunclosed\n");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 4);
    for (const Block& block : list) {
      TEST_CHECK(block.kind == BlockKind::Code);
    }
    TEST_CHECK(text(document, list[0].lang) == "js");
    TEST_CHECK(text(document, list[1].lang).empty());
    TEST_CHECK(text(document, list[2].lang).empty());
    TEST_CHECK(text(document, list[3].lang).empty());
    TEST_CHECK(text(document, list[0].text) == "let *a* = 1;\n# not a heading");
    TEST_CHECK(text(document, list[2].text) == "in ``` here");
    TEST_CHECK(text(document, list[3].text) == "unclosed");
  }

  void testIndentedCode() {
    const Document document = parse("    a\n    b\n\npara");
    TEST_CHECK((kinds(document, document.root) == std::vector{BlockKind::Code, BlockKind::Paragraph}));
    TEST_CHECK(text(document, top(document)[0].text) == "a\nb");
  }

  // The blocks of item `index` of a list block.
  std::vector<Block> itemBlocks(const Document& document, const Block& list, std::size_t index) {
    const std::span<const Range> entries = items(document, list.children);
    TEST_CHECK(index < entries.size());
    const std::span<const Block> found = blocks(document, entries[index]);
    return {found.begin(), found.end()};
  }

  void testLists() {
    const Document document = parse("- a\n- b\n  - b1\n  - b2\n    1. deep\n- c\n\n3. x\n4. y\n");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list[0].kind == BlockKind::List);
    TEST_CHECK(!list[0].ordered);
    TEST_CHECK(list[0].children.count == 3);
    TEST_CHECK(show(document, itemBlocks(document, list[0], 0)[0].runs) == "a");
    const Block inner = itemBlocks(document, list[0], 1)[1];
    TEST_CHECK(inner.kind == BlockKind::List);
    TEST_CHECK(inner.children.count == 2);
    const Block deep = itemBlocks(document, inner, 1)[1];
    TEST_CHECK(deep.kind == BlockKind::List);
    TEST_CHECK(deep.ordered);
    TEST_CHECK(deep.start == 1);
    TEST_CHECK(list[1].ordered);
    TEST_CHECK(list[1].start == 3);
    TEST_CHECK(list[1].children.count == 2);
    TEST_CHECK(!top(parse("* a\n+ b\n")).empty());
    const Document noSpace = parse("-nospace");
    TEST_CHECK(top(noSpace).size() == 1);
    TEST_CHECK(top(noSpace)[0].kind == BlockKind::Paragraph);
  }

  void testQuotes() {
    const Document document = parse("> a\n> b\n>\n> - x\n>> inner\n");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list[0].kind == BlockKind::Quote);
    const std::span<const Block> inside = blocks(document, list[0].children);
    TEST_CHECK(inside.size() == 3);
    TEST_CHECK(show(document, inside[0].runs) == "a b");
    TEST_CHECK(inside[1].kind == BlockKind::List);
    TEST_CHECK(inside[2].kind == BlockKind::Quote);
  }

  // Cell `column` of `row` as compact text; row 0 is the header.
  std::string cellText(const Document& document, const Block& table, std::size_t row, std::size_t column) {
    const std::span<const Range> all = cells(document, table.cells);
    const std::size_t index = (row * table.columns) + column;
    TEST_CHECK(index < all.size());
    return show(document, all[index]);
  }

  void testTables() {
    const Document document = parse("| a | b | c |\n|:--|:-:|--:|\n| 1 | **2** |\n| x\\|y | 4 | 5 | 6 |\n");
    const Block table = top(document)[0];
    TEST_CHECK(table.kind == BlockKind::Table);
    const std::span<const Align> align = alignments(document, table.alignments);
    TEST_CHECK((std::vector(align.begin(), align.end()) == std::vector{Align::Left, Align::Center, Align::Right}));
    TEST_CHECK(table.columns == 3);
    TEST_CHECK(table.rows == 2);
    TEST_CHECK(table.cells.count == 9);
    TEST_CHECK(cellText(document, table, 0, 0) == "a");
    TEST_CHECK(cellText(document, table, 0, 1) == "b");
    TEST_CHECK(cellText(document, table, 0, 2) == "c");
    TEST_CHECK(cellText(document, table, 1, 0) == "1");
    TEST_CHECK(cellText(document, table, 1, 1) == "strong(2)");
    TEST_CHECK(cellText(document, table, 1, 2).empty());
    TEST_CHECK(cellText(document, table, 2, 0) == "x|y");
    TEST_CHECK(cellText(document, table, 2, 1) == "4");
    TEST_CHECK(cellText(document, table, 2, 2) == "5");
    for (const Block& block : top(parse("| a |\n"))) {
      TEST_CHECK(block.kind == BlockKind::Paragraph);
    }
    // Outer pipes are optional.
    TEST_CHECK(top(parse("a | b\n--|--\n1 | 2\n"))[0].kind == BlockKind::Table);
  }

  void testThematicBreaks() {
    const Document document = parse("a\n\n---\n***\n___\n- - -\n");
    TEST_CHECK(
        (kinds(document, document.root)
         == std::vector{BlockKind::Paragraph, BlockKind::Rule, BlockKind::Rule, BlockKind::Rule, BlockKind::Rule})
    );
  }

  void testEmptyInput() {
    TEST_CHECK(top(parse("")).empty());
    TEST_CHECK(top(parse("\n  \n\t\n")).empty());
    TEST_CHECK(!parse("").truncated);
  }

  // Processor time since `start`.
  double millisecondsSince(std::clock_t start) {
    return 1000.0 * static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
  }

  void testCharacterAndLineCaps() {
    TEST_CHECK(parse(repeat("word ", (kMaxChars / 5) + 1000)).truncated);
    const Document lines = parse(repeat("x\n\n", kMaxLines));
    TEST_CHECK(lines.truncated);
    TEST_CHECK(lines.root.count <= kMaxBlocks);
    TEST_CHECK(!parse("short").truncated);
  }

  // Each case is one string of 20,000 repeats, so the input is at most 100,000 characters.
  void testUnclosedMarkersByTheThousand() {
    for (const std::string_view unit : {"*", "_", "**", "`", "[", "![", "](", "[a](", "<", "\\"}) {
      const std::clock_t start = std::clock();
      const Document document = parse(repeat(unit, 20000));
      TEST_CHECK(millisecondsSince(start) < 3000);
      const std::vector<Block> list = top(document);
      TEST_CHECK(list.size() == 1);
      // A line of only markers may be a thematic break or an unclosed fence.
      TEST_CHECK(
          (list[0].kind == BlockKind::Paragraph || list[0].kind == BlockKind::Rule || list[0].kind == BlockKind::Code)
      );
    }
  }

  void testAlternatingOpenersStayLinear() {
    const std::vector<std::string> cases{
        repeat("*a ", 10000) + "*", repeat("[a ", 10000) + "](x)", repeat("`a``b```c", 5000), repeat("[a](b)", 5000),
        repeat("*a* ", 10000)
    };
    for (const std::string& source : cases) {
      const std::clock_t start = std::clock();
      const Document document = parse(source);
      TEST_CHECK(millisecondsSince(start) < 3000);
      TEST_CHECK(!top(document).empty());
    }
  }

  void testDeepBlockNestingIsFlattened() {
    for (const std::string_view unit : {"> ", "- ", "  - ", "1. "}) {
      const Document document = parse(repeat(unit, 5000) + "x");
      std::size_t depth = 0;
      std::vector<Block> level = top(document);
      while (!level.empty() && (level[0].kind == BlockKind::Quote || level[0].kind == BlockKind::List) && depth < 100) {
        if (level[0].kind == BlockKind::Quote) {
          const std::span<const Block> inside = blocks(document, level[0].children);
          level.assign(inside.begin(), inside.end());
        } else {
          level = itemBlocks(document, level[0], 0);
        }
        ++depth;
      }
      TEST_CHECK(depth <= kMaxDepth);
      TEST_CHECK(document.truncated);
    }
    std::string lines;
    for (std::size_t k = 0; k < 3000; ++k) {
      lines += std::string(k % 40, ' ') + "- item\n";
    }
    TEST_CHECK(!top(parse(lines)).empty());
  }

  // Deepest nesting of runs of the given kinds. Recursion is bounded by the parser's own limits.
  std::size_t nesting(const Document& document, Range range, std::span<const RunKind> counted, std::size_t depth = 0) {
    std::size_t deepest = depth;
    for (const Run& run : runs(document, range)) {
      if (run.children.count == 0) {
        continue;
      }
      const std::size_t inner = std::ranges::contains(counted, run.kind) ? depth + 1 : depth;
      deepest = std::max({deepest, inner, nesting(document, run.children, counted, inner)});
    }
    return deepest;
  }

  constexpr std::array kEmphasisKinds{RunKind::Emphasis, RunKind::Strong};
  constexpr std::array kLinkKinds{RunKind::Link};

  std::size_t emphasisNesting(std::string_view source) {
    const Document document = parse(source);
    TEST_CHECK(!top(document).empty());
    TEST_CHECK(top(document)[0].kind == BlockKind::Paragraph);
    return nesting(document, top(document)[0].runs, kEmphasisKinds);
  }

  void testEmphasisDepthLimit() {
    TEST_CHECK(kMaxInlineDepth == 16);
    // 1,990 stars opening, then closers one pair at a time: about 2,000 levels without the bound.
    const std::string pairwise = repeat("*", 1990) + "a\n" + repeat("a*", 995) + "\n";
    const std::string tall = repeat("*", 1990) + "a\n" + repeat("a*\n", 995);
    // Eight lines of 1,999 stars, then 17 lines of closers: about 16,000 levels without the bound.
    const std::string huge = repeat(repeat("*", 1999) + "a\n", 8) + repeat(repeat("a*", 995) + "\n", 17);
    const std::string mixed = repeat("**a *b ", 40) + "x" + repeat(" b* a**", 40);
    for (const std::string& source : {pairwise, tall, huge, mixed}) {
      TEST_CHECK(emphasisNesting(source) <= kMaxInlineDepth);
    }
    // A nesting within the bound is kept as written.
    TEST_CHECK(emphasisNesting("*a **b *c **d *e **f *g **h x h** g* f** e* d** c* b** a*") == 8);
    TEST_CHECK(emphasisNesting("***x***") == 2);
  }

  // One paragraph that leaves a '_' and a '*' opener stuck below 16 nested levels, then makes most of the tail a
  // refused closing run.
  std::string stuckOpeners(std::size_t tailChars) {
    const std::string head = "_w *w " + repeat("*q ", 16) + "z" + repeat("z*", 16) + " ";
    const std::string tail = repeat("_*", tailChars / 2);
    std::string out = head;
    for (std::size_t start = 0; start < tail.size(); start += 1800) {
      if (start > 0) {
        out += '\n';
      }
      out += tail.substr(start, 1800);
    }
    return out;
  }

  void testRefusedClosersCostConstantWork() {
    const Document small = parse(stuckOpeners(12000));
    const Document big = parse(stuckOpeners(48000));
    // Both inputs are inside the limits, so nothing was cut.
    TEST_CHECK(!small.truncated);
    TEST_CHECK(!big.truncated);
    // Four times the input is about four times the work; a quadratic scan gives about 16.
    const double ratio = static_cast<double>(big.emphasisSteps) / static_cast<double>(small.emphasisSteps);
    TEST_CHECK(ratio > 3);
    TEST_CHECK(ratio < 6);
    TEST_CHECK(big.emphasisSteps < 48000 * 200);
    TEST_CHECK(small.emphasisSteps == 40231);
    TEST_CHECK(big.emphasisSteps == 160191);
    const std::clock_t start = std::clock();
    for (std::size_t k = 0; k < 4; ++k) {
      TEST_CHECK(!parse(stuckOpeners(48000)).truncated);
    }
    TEST_CHECK(millisecondsSince(start) < 1500);
  }

  void testOpenerCap() {
    const Document document = parse(repeat("*a ", kMaxOpeners + 6) + "a*");
    const Run* em = nullptr;
    for (const Run& run : runs(document, top(document)[0].runs)) {
      if (run.kind == RunKind::Emphasis) {
        em = &run;
        break;
      }
    }
    TEST_CHECK(em != nullptr);
    // The six openers past the cap are literal text inside the one emphasis that did match.
    std::size_t literal = 0;
    for (const Run& run : runs(document, em->children)) {
      if (run.kind == RunKind::Text) {
        literal += static_cast<std::size_t>(std::ranges::count(text(document, run.text), '*'));
      }
    }
    TEST_CHECK(literal == 6);
  }

  std::size_t countLinks(const Document& document, Range range) {
    std::size_t total = 0;
    for (const Run& run : runs(document, range)) {
      total += (run.kind == RunKind::Link ? 1 : 0) + countLinks(document, run.children);
    }
    return total;
  }

  void testLinkWorkBudget() {
    // 12,000 unclosed brackets over 20 lines, each line under the line cap.
    const Document document = parse(repeat(repeat("[a ", 600) + "\n", 20) + "[x](http://y)");
    TEST_CHECK(countLinks(document, top(document)[0].runs) == 0);
    // Small inputs are unaffected.
    const Document small = parse("[x](http://y) and [z](w)");
    TEST_CHECK(countLinks(small, top(small)[0].runs) == 2);
  }

  void testLinkLabelDepth() {
    const Document document = parse(repeat("[", 10) + "x" + repeat("](u)", 10));
    const std::size_t depth = nesting(document, top(document)[0].runs, kLinkKinds);
    TEST_CHECK(depth >= 1);
    TEST_CHECK(depth <= kMaxLabelDepth + 1);
  }

  void testListAndTableCaps() {
    const Document list = parse(repeat("- x\n", kMaxRows + 100));
    TEST_CHECK(top(list)[0].children.count == kMaxRows);
    const std::string row = "|" + repeat(" c |", 500);
    const Document wide = parse(row + "\n|" + repeat("-|", 500) + "\n" + row + "\n");
    const Block table = top(wide)[0];
    TEST_CHECK(table.kind == BlockKind::Table);
    TEST_CHECK(table.columns <= kMaxColumns);
    TEST_CHECK(table.cells.count == table.columns * (table.rows + 1));
    TEST_CHECK(table.rows == 1);
    TEST_CHECK(table.columns == kMaxColumns);
  }

  void testScriptTagsStayInertEverywhere() {
    const Document document = parse(
        "# <script>x</script>\n> <script>x</script>\n- <script>x</script>\n| <script>x</script> |\n|---|\n"
        "| [a](java"
        "script:x) |\n"
    );
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 3);
    TEST_CHECK(show(document, list[0].runs) == "<script>x</script>");
    TEST_CHECK(show(document, blocks(document, list[1].children)[0].runs) == "<script>x</script>");
    // The pipe lines continue the list item's paragraph; the link target is data.
    TEST_CHECK(
        show(document, itemBlocks(document, list[2], 0)[0].runs)
        == "<script>x</script> | <script>x</script> | |---| | link[java"
           "script:x](a) |"
    );
  }

  void testSpansPointAtInputBytes() {
    const Document document = parse("a **b** `c`\n\n```\nx\ny\n```\n");
    TEST_CHECK(document.sourceSize == 25);
    const std::vector<Block> list = top(document);
    const std::span<const Run> line = runs(document, list[0].runs);
    TEST_CHECK(line.size() == 4);
    TEST_CHECK((line[0].text == Span{.offset = 0, .length = 2}));
    TEST_CHECK(line[1].kind == RunKind::Strong);
    TEST_CHECK((runs(document, line[1].children)[0].text == Span{.offset = 4, .length = 1}));
    TEST_CHECK((line[2].text == Span{.offset = 7, .length = 1}));
    TEST_CHECK((line[3].text == Span{.offset = 9, .length = 1}));
    TEST_CHECK((list[1].text == Span{.offset = 17, .length = 3}));
    // Through a quote marker on the second line, a run still points at its input bytes.
    const Document quoted = parse("> x\n> *y*");
    const Block paragraph = blocks(quoted, top(quoted)[0].children)[0];
    const std::span<const Run> quotedRuns = runs(quoted, paragraph.runs);
    TEST_CHECK(quotedRuns.size() == 2);
    TEST_CHECK((runs(quoted, quotedRuns[1].children)[0].text == Span{.offset = 7, .length = 1}));
  }

  void testTextTheParserMadeFollowsTheInput() {
    const Document joined = parse("a\nb");
    const Run run = runs(joined, top(joined)[0].runs)[0];
    TEST_CHECK((run.text == Span{.offset = 3, .length = 3}));
    TEST_CHECK(text(joined, run.text) == "a b");
    const Document tabs = parse("\tcode\n\t\tmore\n");
    const Block code = top(tabs)[0];
    TEST_CHECK(code.kind == BlockKind::Code);
    TEST_CHECK(text(tabs, code.text) == "code\n    more");
    TEST_CHECK(code.text.offset >= tabs.sourceSize);
  }

  void testLineEndings() {
    const Document document = parse("- a\r\n- b\r\n\r\npara\rnext");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 2);
    TEST_CHECK(list[0].children.count == 2);
    TEST_CHECK(show(document, itemBlocks(document, list[0], 1)[0].runs) == "b");
    TEST_CHECK(show(document, list[1].runs) == "para next");
    TEST_CHECK(para("`a\nb`") == "code(a b)");
  }

  void testUnicodeSpaceAndLetters() {
    // A line of no-break spaces is blank, and trailing ones are trimmed.
    const Document document = parse("x \n \n\ny");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 2);
    TEST_CHECK(show(document, list[0].runs) == "x");
    TEST_CHECK(show(document, list[1].runs) == "y");
    // Letters outside ASCII make an underscore intraword.
    TEST_CHECK(para("café_x_ and _été_") == "café_x_ and em(été)");
  }

  // A character above U+FFFF counts two units, so it can straddle a limit; the cut then drops all of it.
  void testCutsKeepWholeCharacters() {
    const Document fits = parse(std::string(kMaxLine - 1, 'a') + "é");
    TEST_CHECK(!fits.truncated);
    TEST_CHECK(show(fits, top(fits)[0].runs) == std::string(kMaxLine - 1, 'a') + "é");
    const Document line = parse(std::string(kMaxLine - 1, 'a') + "😀");
    TEST_CHECK(line.truncated);
    TEST_CHECK(show(line, top(line)[0].runs) == std::string(kMaxLine - 1, 'a'));
    const Document input = parse(repeat(std::string(999, 'a') + "\n", kMaxChars / 1000) + "é");
    TEST_CHECK(input.truncated);
    TEST_CHECK(input.sourceSize == kMaxChars);
    const std::string head = repeat(std::string(999, 'a') + "\n", (kMaxChars / 1000) - 1) + std::string(999, 'a');
    // The input is read whole; one paragraph this long is still cut at kMaxInline.
    const Document whole = parse(head + "é");
    TEST_CHECK(whole.sourceSize == kMaxChars + 1);
    const Document cut = parse(head + "😀");
    TEST_CHECK(cut.truncated);
    TEST_CHECK(cut.sourceSize == kMaxChars - 1);
  }

  // Lengths count UTF-16 units: a three-byte character counts one, so text outside ASCII gets the same room.
  void testLimitsCountTextUnits() {
    const std::string wide = repeat("日", 702);
    const Document line = parse(wide);
    TEST_CHECK(!line.truncated);
    TEST_CHECK(show(line, top(line)[0].runs) == wide);
    const std::string shortLabel = repeat("日", 120);
    TEST_CHECK(para("[" + shortLabel + "](u)") == "link[u](" + shortLabel + ")");
    // A label closes within kMaxLabel units of its bracket, a target within kMaxTarget units of its parenthesis.
    const std::string label = repeat("日", kMaxLabel - 1);
    TEST_CHECK(para("[" + label + "](u)") == "link[u](" + label + ")");
    TEST_CHECK(para("[" + label + "日](u)") == "[" + label + "日](u)");
    const std::string pairs = repeat("😀", (kMaxLabel - 1) / 2);
    TEST_CHECK(para("[" + pairs + "](u)") == "link[u](" + pairs + ")");
    TEST_CHECK(para("[" + pairs + "😀](u)") == "[" + pairs + "😀](u)");
    const std::string target = repeat("日", kMaxTarget - 1);
    TEST_CHECK(para("[a](" + target + ")") == "link[" + target + "](a)");
    TEST_CHECK(para("[a](" + target + "日)") == "[a](" + target + "日)");
    // 18 lines of 1,000 characters, 18,000 units, stay under kMaxInline.
    const std::string row = repeat("日", 1000);
    const Document paragraph = parse(repeat(row + "\n", 17) + row);
    TEST_CHECK(!paragraph.truncated);
    TEST_CHECK(show(paragraph, top(paragraph)[0].runs) == repeat(row + " ", 17) + row);
    // 30 lines of 1,999 characters: the cut keeps 25 lines and their line ends, 50,000 units in 149,950 bytes.
    const Document cut = parse(repeat(repeat("日", 1999) + "\n", 30));
    TEST_CHECK(cut.truncated);
    TEST_CHECK(text(cut, runs(cut, top(cut)[0].runs)[0].text) == repeat(repeat("日", 1999) + " ", 25));
    // 7,800 unclosed brackets scan 300 units each, 2,340,000 of the kWork budget, so the last link still forms.
    const Document budget = parse(repeat(repeat("[日 ", 600) + "\n", 13) + "[x](http://y)");
    TEST_CHECK(countLinks(budget, top(budget)[0].runs) == 1);
  }

  // Text joined from bytes that are not adjacent in the input is stored as made text.
  void testSpansClaimOnlyAdjacentBytes() {
    // Each kept backslash is the second of an escaped pair.
    const Document pairs = parse(R"(\\\\)");
    const std::span<const Run> line = runs(pairs, top(pairs)[0].runs);
    TEST_CHECK(line.size() == 1);
    TEST_CHECK(text(pairs, line[0].text) == R"(\\)");
    TEST_CHECK(line[0].text.offset >= pairs.sourceSize);
    const Document adjacent = parse("\\*b");
    TEST_CHECK((runs(adjacent, top(adjacent)[0].runs)[0].text == Span{.offset = 1, .length = 2}));
    // Past the depth cap the second line follows twelve stripped bytes of quote markers that repeat its text.
    const Document deep = parse(repeat("> ", kMaxDepth) + "x\n" + repeat("> ", 18));
    Block level = top(deep)[0];
    for (std::size_t k = 0; k < kMaxDepth; ++k) {
      level = blocks(deep, level.children)[0];
    }
    const Run flat = runs(deep, level.runs)[0];
    TEST_CHECK(text(deep, flat.text) == "x\n" + repeat("> ", 11) + ">");
    TEST_CHECK(flat.text.offset >= deep.sourceSize);
  }

  void testLineCapBoundary() {
    TEST_CHECK(!parse(repeat("x\n", kMaxLines)).truncated);
    TEST_CHECK(parse(repeat("x\n", kMaxLines) + "y").truncated);
    TEST_CHECK(!parse(repeat("x\n", kMaxLines - 1) + "y\n").truncated);
  }

  void testBlockRowAndInlineCaps() {
    const Document rules = parse(repeat("---\n", 3500));
    TEST_CHECK(rules.root.count == kMaxBlocks);
    TEST_CHECK(rules.truncated);
    const Document table = parse("|a|\n|-|\n" + repeat("|r|\n", kMaxRows + 5));
    TEST_CHECK(top(table).size() == 1);
    TEST_CHECK(top(table)[0].rows == kMaxRows);
    TEST_CHECK(table.truncated);
    const Document wide = parse(repeat(std::string(1999, 'a') + "\n", 30));
    TEST_CHECK(wide.truncated);
    TEST_CHECK(text(wide, runs(wide, top(wide)[0].runs)[0].text).size() == kMaxInline);
  }

  void testTooDeepTextIsPlain() {
    const Document document = parse(repeat("> ", 10) + "*x*");
    TEST_CHECK(document.truncated);
    Block level = top(document)[0];
    for (std::size_t k = 0; k < kMaxDepth; ++k) {
      TEST_CHECK(level.kind == BlockKind::Quote);
      level = blocks(document, level.children)[0];
    }
    TEST_CHECK(level.kind == BlockKind::Paragraph);
    TEST_CHECK(show(document, level.runs) == "> > > > *x*");
  }

  void testCodeSpanEdgeSpaces() {
    TEST_CHECK(para("`` `x` ``") == "code(`x`)");
    TEST_CHECK(para("` a `") == "code(a)");
  }

  void testEscapesInLabelsAndTargets() {
    TEST_CHECK(para("[a\\]b](c\\)d)") == "link[c\\)d](a]b)");
    // The target ends at its first space; a title after it is dropped.
    TEST_CHECK(para("[a](  b  c )") == "link[b](a)");
  }

  void testContinuationLines() {
    const Document quote = parse("> a\nb");
    TEST_CHECK(top(quote).size() == 1);
    TEST_CHECK(show(quote, blocks(quote, top(quote)[0].children)[0].runs) == "a b");
    const Document item = parse("- a\n\n  b");
    const std::vector<Block> inside = itemBlocks(item, top(item)[0], 0);
    TEST_CHECK(inside.size() == 2);
    TEST_CHECK(show(item, inside[1].runs) == "b");
  }

  void testMalformedDelimiterRowsAreText() {
    TEST_CHECK(para("|a|\n|:|\n") == "|a| |:|");
    TEST_CHECK(para("a|b|c\n-|-\n") == "a|b|c -|-");
    TEST_CHECK(para("a | b\n---c") == "a | b ---c");
  }

  void testBlankTextPastTheDepthCap() {
    const Document document = parse(repeat("> ", kMaxDepth));
    TEST_CHECK(document.truncated);
    Block level = top(document)[0];
    for (std::size_t k = 1; k < kMaxDepth; ++k) {
      level = blocks(document, level.children)[0];
    }
    TEST_CHECK(level.kind == BlockKind::Quote);
    TEST_CHECK(level.children.count == 0);
  }

  void testThreeByteSpaces() {
    const Document document = parse("x\u2003\u3000\u2028\n\u205F\uFEFF\u1680\u2009\n\ny");
    const std::vector<Block> list = top(document);
    TEST_CHECK(list.size() == 2);
    TEST_CHECK(show(document, list[0].runs) == "x");
    TEST_CHECK(show(document, list[1].runs) == "y");
  }

  void testAccessorsOutsideTheDocument() {
    const Document document = parse("a");
    TEST_CHECK(text(document, Span{.offset = 0, .length = 2}).empty());
    TEST_CHECK(text(document, Span{.offset = 5, .length = 0}).empty());
    TEST_CHECK(blocks(document, Range{.first = 0, .count = 2}).empty());
    TEST_CHECK(runs(document, Range{.first = 1, .count = 1}).empty());
    TEST_CHECK(items(document, Range{.first = 0, .count = 1}).empty());
    TEST_CHECK(cells(document, Range{.first = 0, .count = 1}).empty());
    TEST_CHECK(alignments(document, Range{.first = 0, .count = 1}).empty());
    TEST_CHECK(blocks(document, Range{.first = 1, .count = static_cast<std::size_t>(-1)}).empty());
    TEST_CHECK(text(document, Span{.offset = 1, .length = static_cast<std::size_t>(-1)}).empty());
  }
} // namespace

int main() {
  testHeadings();
  testParagraphs();
  testEmphasis();
  testCodeSpans();
  testLinksAndImagesAreData();
  testRawHtmlIsText();
  testFencedCode();
  testIndentedCode();
  testLists();
  testQuotes();
  testTables();
  testThematicBreaks();
  testEmptyInput();
  testCharacterAndLineCaps();
  testUnclosedMarkersByTheThousand();
  testAlternatingOpenersStayLinear();
  testDeepBlockNestingIsFlattened();
  testEmphasisDepthLimit();
  testRefusedClosersCostConstantWork();
  testOpenerCap();
  testLinkWorkBudget();
  testLinkLabelDepth();
  testListAndTableCaps();
  testScriptTagsStayInertEverywhere();
  testSpansPointAtInputBytes();
  testTextTheParserMadeFollowsTheInput();
  testLineEndings();
  testUnicodeSpaceAndLetters();
  testCutsKeepWholeCharacters();
  testLimitsCountTextUnits();
  testSpansClaimOnlyAdjacentBytes();
  testLineCapBoundary();
  testBlockRowAndInlineCaps();
  testTooDeepTextIsPlain();
  testCodeSpanEdgeSpaces();
  testEscapesInLabelsAndTargets();
  testContinuationLines();
  testMalformedDelimiterRowsAreText();
  testBlankTextPastTheDepthCap();
  testThreeByteSpaces();
  testAccessorsOutsideTheDocument();
  std::println("formats markdown checks passed");
  return 0;
}
