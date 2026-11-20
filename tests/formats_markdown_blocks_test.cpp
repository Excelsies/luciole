#include "formats/markdown.h"
#include "formats/markdown_blocks.h"
#include "formats/markdown_text.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats::markdown;

  // The lines of `input` split at '\n', each mapped to its input offset.
  std::vector<Line> linesOf(std::string_view input) {
    std::vector<Line> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= input.size(); ++i) {
      if (i == input.size() || input[i] == '\n') {
        lines.push_back({.text = input.substr(start, i - start), .mappedFrom = 0, .origin = start});
        start = i + 1;
      }
    }
    return lines;
  }

  void testBlocks() {
    constexpr std::string_view input = "# Title\n\npara\ntext\n\n- one\n- two\n\n> quote\n\n```sh\nls\n```";
    Document document;
    document.text = std::string(input);
    document.sourceSize = input.size();
    Context context{.document = document, .expanded = {}};
    const auto lines = linesOf(input);
    const auto top = blocks(document, parseBlocks(context, lines, 0));
    TEST_CHECK(top.size() == 5);
    TEST_CHECK(top[0].kind == BlockKind::Heading && top[0].level == 1);
    TEST_CHECK(top[1].kind == BlockKind::Paragraph);
    // A line end inside a paragraph reads as one space.
    TEST_CHECK(text(document, runs(document, top[1].runs)[0].text) == "para text");
    TEST_CHECK(top[2].kind == BlockKind::List && top[2].children.count == 2 && !top[2].ordered);
    TEST_CHECK(top[3].kind == BlockKind::Quote);
    TEST_CHECK(top[4].kind == BlockKind::Code && text(document, top[4].lang) == "sh");
    TEST_CHECK(text(document, top[4].text) == "ls");
    TEST_CHECK(!document.truncated);
  }

  void testDepthLimit() {
    constexpr std::string_view input = "> > a\nb";
    Document document;
    document.text = std::string(input);
    document.sourceSize = input.size();
    Context context{.document = document, .expanded = {}};
    const auto top = blocks(document, parseBlocks(context, linesOf(input), kMaxDepth));
    TEST_CHECK(top.size() == 1 && top[0].kind == BlockKind::Paragraph);
    TEST_CHECK(document.truncated);
    TEST_CHECK(blocks(document, parseBlocks(context, {}, 0)).empty());
  }
} // namespace

int main() {
  testBlocks();
  testDepthLimit();
  std::println("formats markdown blocks checks passed");
  return 0;
}
