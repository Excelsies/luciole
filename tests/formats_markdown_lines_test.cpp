#include "formats/markdown_lines.h"
#include "formats/markdown_text.h"
#include "test_check.h"

#include <print>
#include <string_view>

namespace {
  using namespace luciole::formats::markdown;

  Line line(std::string_view text) { return {.text = text, .mappedFrom = 0, .origin = 0}; }

  void testFences() {
    const auto fence = fenceOf(line("  ```cpp extra"));
    TEST_CHECK(fence && fence->ch == '`' && fence->count == 3 && fence->indent == 2 && fence->lang.text == "cpp");
    TEST_CHECK(fence->lang.origin == 5);
    TEST_CHECK(fenceOf(line("~~~~")) && fenceOf(line("~~~~"))->count == 4);
    TEST_CHECK(!fenceOf(line("``")));
    TEST_CHECK(!fenceOf(line("    ```")));
    TEST_CHECK(!fenceOf(line("``` a`b")));
    TEST_CHECK(fenceOf(line("~~~ a`b")));
  }

  void testHeadings() {
    const auto atx = atxOf(line("## Title ##"));
    TEST_CHECK(atx && atx->level == 2 && atx->text.text == "Title");
    TEST_CHECK(atxOf(line("#")) && atxOf(line("#"))->text.text.empty());
    TEST_CHECK(atxOf(line("# a#")) && atxOf(line("# a#"))->text.text == "a#");
    TEST_CHECK(!atxOf(line("#hash")));
    TEST_CHECK(!atxOf(line("####### seven")));
    TEST_CHECK(!atxOf(line("    # code")));
  }

  void testRulesAndRuns() {
    TEST_CHECK(isRule("* * *") && isRule("---") && isRule("  _\t_ _"));
    TEST_CHECK(!isRule("--") && !isRule("-*-") && !isRule("    ---") && !isRule(""));
    TEST_CHECK(allOf("===", '=') && allOf("  --  ", '-'));
    TEST_CHECK(!allOf("= =", '=') && !allOf("", '=') && !allOf("    ===", '='));
  }

  void testMarkers() {
    const auto bullet = markerOf("-  item");
    TEST_CHECK(bullet && !bullet->ordered && bullet->bullet == '-' && bullet->content == 3);
    const auto ordered = markerOf(" 12) x");
    TEST_CHECK(ordered && ordered->ordered && ordered->number == 12 && ordered->bullet == ')');
    TEST_CHECK(ordered->indent == 1 && ordered->content == 5);
    TEST_CHECK(markerOf("+") && markerOf("+")->content == 2);
    TEST_CHECK(!markerOf("-x") && !markerOf("1.x") && !markerOf("x. y") && !markerOf("1234567890. y"));
    // A gap wider than four spaces means indented content: the item text starts one space after the marker.
    TEST_CHECK(markerOf("-      code")->content == 2);
    TEST_CHECK(startsQuote("   > a") && !startsQuote("    > a") && !startsQuote("a > b"));
  }

  void testInterrupts() {
    TEST_CHECK(interrupts(line("```")) && interrupts(line("# h")) && interrupts(line("> q")));
    TEST_CHECK(interrupts(line("***")) && interrupts(line("- item")) && interrupts(line("1. item")));
    TEST_CHECK(!interrupts(line("2. item")) && !interrupts(line("-")) && !interrupts(line("text")));
  }
} // namespace

int main() {
  testFences();
  testHeadings();
  testRulesAndRuns();
  testMarkers();
  testInterrupts();
  std::println("formats markdown lines checks passed");
  return 0;
}
