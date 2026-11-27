#include "formats/styled_text.h"
#include "imaging/text_tile_renderer.h"
#include "test_check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>

// Plain text written to be read wraps at word boundaries and is drawn in the code family at the code size; the
// prose of a Markdown document stays in the interface family. Glyph advances come from the layout's own boxes.
namespace {
  using namespace luciole;

  constexpr double kTextSize = 14.0;
  constexpr double kCodeScale = 0.95;
  constexpr double kPadding = 8.0; // logical pixels around a document

  std::shared_ptr<const formats::StyledDocument> document(std::string_view text, std::string_view name) {
    return std::make_shared<const formats::StyledDocument>(formats::styledText(text, formats::fileKind(name)));
  }

  TextTileOptions options(double width, double scale = 1.0) {
    TextTileOptions out{.width = width, .scale = scale};
    out.fonts.proseSize = kTextSize;
    out.fonts.codeSize = kTextSize * kCodeScale;
    return out;
  }

  TextBox box(TextTileLayout& layout, std::size_t line, std::size_t column) {
    const auto found = layout.characterBox({.block = 0, .line = line, .column = column});
    TEST_CHECK(found.has_value());
    return found.value_or(TextBox{});
  }

  // "i" is narrow and "m" wide in a proportional family; in a monospace family they take the same advance.
  void plainTextHasEqualAdvances() {
    const std::string text = "iiiiiiii mmmmmmmm";
    TextTileLayout plain(document(text, "notes.txt"), options(600.0));
    const auto narrow = box(plain, 0, 0);
    const auto wide = box(plain, 0, 9);
    std::println("plain text: i {:.3f} wide, m {:.3f} wide", narrow.width, wide.width);
    TEST_NEAR(narrow.width, wide.width, 0.01);
    TEST_NEAR(box(plain, 0, 1).x - narrow.x, box(plain, 0, 10).x - wide.x, 0.01);
    // The same words in Markdown stay in the interface family.
    TextTileLayout markdown(document(text, "notes.md"), options(600.0));
    TEST_CHECK(box(markdown, 0, 9).width > box(markdown, 0, 0).width * 1.5);
  }

  // The family, size and line height are the code lines' own: the same characters in a log file give the same boxes.
  void plainTextHasTheCodeFace() {
    const std::string text = "iiii mmmm 0123 WWWW";
    for (const double scale : {1.0, 1.5}) {
      TextTileLayout plain(document(text, "notes.txt"), options(600.0, scale));
      TextTileLayout code(document(text, "notes.log"), options(600.0, scale));
      for (const std::size_t column : {0UL, 5UL, 10UL, 15UL}) {
        const auto a = box(plain, 0, column);
        const auto b = box(code, 0, column);
        TEST_NEAR(a.x, b.x, 0.01);
        TEST_NEAR(a.width, b.width, 0.01);
        TEST_NEAR(a.height, b.height, 0.01);
      }
    }
  }

  struct Rows {
    std::size_t count = 0;
    std::size_t atWordStarts = 0; // rows after the first of a line whose first character follows a space
    std::size_t continued = 0;    // rows after the first of a line
  };

  // The rows the lines of the document wrap to, counted from where their characters are.
  Rows rowsOf(TextTileLayout& layout, const formats::StyledDocument& doc) {
    Rows rows;
    for (std::size_t line = 0; line < doc.blocks[0].lines.size(); ++line) {
      const auto text = formats::joinedText(doc.blocks[0].lines[line]);
      std::set<long> tops;
      std::optional<double> previous;
      for (std::size_t column = 0; column < std::max<std::size_t>(text.size(), 1); ++column) {
        const double y = box(layout, line, column).y;
        if (previous && y > *previous + 1.0) {
          ++rows.continued;
          rows.atWordStarts += text[column - 1] == ' ' ? 1 : 0;
        }
        previous = y;
        tops.insert(std::lround(y));
      }
      rows.count += tops.size();
    }
    return rows;
  }

  // At a narrow width the text wraps at word boundaries and the layout is as tall as its rows of the font's height.
  void wrapsAtWordsAndHeightFollowsTheRows() {
    std::string text;
    for (const auto* word :
         {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet"}) {
      text += std::format("{} ", word);
    }
    text = text + text + text + "\n\nshort\n";
    const auto doc = document(text, "notes.txt");
    for (const double scale : {1.0, 1.5}) {
      TextTileLayout narrow(doc, options(150.0, scale));
      TextTileLayout wide(doc, options(3000.0, scale));
      const auto rows = rowsOf(narrow, *doc);
      const auto height = box(narrow, 0, 0).height;
      std::println(
          "{}x: {} rows, {} continued, {} at word starts, row {:.2f}, layout {}", scale, rows.count, rows.continued,
          rows.atWordStarts, height, narrow.height()
      );
      TEST_CHECK(rows.continued >= 5);
      TEST_CHECK(rows.atWordStarts == rows.continued);
      // The first line, a blank line and a short one: the first wraps to the rows counted, the others take one each.
      TEST_NEAR(
          narrow.height(), (2 * kPadding * scale) + (static_cast<double>(rows.count) * height),
          0.5 * static_cast<double>(rows.count)
      );
      TEST_CHECK(rowsOf(wide, *doc).count == 3 && rows.count > 5);
      TEST_CHECK(narrow.width() == static_cast<int>(std::lround(150.0 * scale)));
    }
  }

  // A word longer than the width still breaks inside itself, and every character stays inside the layout.
  void longWordsStillBreak() {
    const std::string text = std::string(300, 'x') + "\nend";
    TextTileLayout layout(document(text, "notes.txt"), options(200.0));
    const auto first = box(layout, 0, 0);
    const auto last = box(layout, 0, 299);
    TEST_CHECK(last.y > first.y + first.height);
    TEST_CHECK(last.x + last.width <= 200.0 + 0.5);
    // Monospace: the 200 pixels hold the same number of characters on every full row.
    TEST_NEAR(box(layout, 0, 40).y, box(layout, 0, 41).y, 1.0);
    TextTileLayout code(document(text, "notes.log"), options(200.0));
    TEST_CHECK(code.width() > 200);
  }
} // namespace

int main() {
  plainTextHasEqualAdvances();
  plainTextHasTheCodeFace();
  wrapsAtWordsAndHeightFollowsTheRows();
  longWordsStillBreak();
  std::println("text-tile-plain-font: all checks passed");
  return 0;
}
