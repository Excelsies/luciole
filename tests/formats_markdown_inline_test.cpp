#include "formats/markdown.h"
#include "formats/markdown_inline.h"
#include "formats/markdown_text.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::formats::markdown;

  // A document holding `input` as its source, the way parse sets one up.
  Document documentOf(std::string_view input) {
    Document document;
    document.text = std::string(input);
    document.sourceSize = input.size();
    return document;
  }

  void testRuns() {
    constexpr std::string_view input = "a *b* `c` [d](e)";
    auto document = documentOf(input);
    Context context{.document = document, .expanded = {}};
    const auto range = parseInline(context, sourceOf({.text = input, .mappedFrom = 0, .origin = 0}), 0);
    const auto top = runs(document, range);
    TEST_CHECK(top.size() == 6);
    TEST_CHECK(top[0].kind == RunKind::Text && text(document, top[0].text) == "a ");
    TEST_CHECK(top[1].kind == RunKind::Emphasis && text(document, runs(document, top[1].children)[0].text) == "b");
    TEST_CHECK(top[3].kind == RunKind::Code && text(document, top[3].text) == "c");
    TEST_CHECK(top[5].kind == RunKind::Link && text(document, top[5].text) == "e");
    TEST_CHECK(text(document, runs(document, top[5].children)[0].text) == "d");
    // Every text here is a range of the input.
    TEST_CHECK(document.text == input);
    TEST_CHECK((top[0].text == Span{.offset = 0, .length = 2}));
  }

  void testSourceOfKeepsOffsets() {
    const auto source = sourceOf({.text = "    x", .mappedFrom = 3, .origin = 20});
    TEST_CHECK(source.text == "    x");
    TEST_CHECK(source.origin[0] == kNoOrigin && source.origin[3] == 20 && source.origin[4] == 21);
  }

  // Below kMaxLabelDepth a link label is inline text of its own; at the limit it is one plain text run.
  void testLabelDepthStopsLabelParsing() {
    constexpr std::string_view input = "[*a*](b)";
    auto document = documentOf(input);
    Context context{.document = document, .expanded = {}};
    const auto source = sourceOf({.text = input, .mappedFrom = 0, .origin = 0});
    const auto parsed = runs(document, parseInline(context, source, 0));
    TEST_CHECK(parsed.size() == 1 && parsed[0].kind == RunKind::Link);
    TEST_CHECK(runs(document, parsed[0].children)[0].kind == RunKind::Emphasis);
    const auto flat = runs(document, parseInline(context, source, kMaxLabelDepth));
    TEST_CHECK(flat.size() == 1 && flat[0].kind == RunKind::Link);
    const auto label = runs(document, flat[0].children);
    TEST_CHECK(label.size() == 1 && label[0].kind == RunKind::Text && text(document, label[0].text) == "*a*");
  }
} // namespace

int main() {
  testRuns();
  testSourceOfKeepsOffsets();
  testLabelDepthStopsLabelParsing();
  std::println("formats markdown inline checks passed");
  return 0;
}
