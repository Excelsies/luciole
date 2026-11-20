#include "test_check.h"
#include "workbench/entry_text.h"
#include "workbench/nest_labels.h"
#include "workbench_nest_fixture.h"

#include <cstddef>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::workbench;
  using namespace luciole::test::nest;

  // Width of a text at 7 px per code point.
  double fixedWidth(std::string_view text) {
    double count = 0;
    for (const char c : text) {
      if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
        ++count;
      }
    }
    return count * 7.0;
  }

  void tileLabelsFollowTheSizeRules() {
    auto fx = fixture();
    const auto folder = fx.find("l3");
    auto label = nestTileLabel(fx.tree, kSettings, folder, {.x = 10, .y = 20, .w = 220, .h = 100});
    TEST_CHECK(label.has_value() && label->name == "l3" && label->detail == "87.9\u00A0KiB  ·  7 items");
    TEST_CHECK(label->fontSize == 15 && label->iconSize == 14 && label->twoLines);
    TEST_NEAR(label->nameAt.x, 38.0, kEps);
    TEST_NEAR(label->nameAt.y, 43.0, kEps);
    TEST_NEAR(label->nameWidth, 184.0, kEps);
    TEST_NEAR(label->icon.x, 18.0, kEps);
    TEST_NEAR(label->icon.y, 27.5, kEps);
    TEST_NEAR(label->detailAt.x, 18.0, kEps);
    TEST_NEAR(label->detailAt.y, 60.0, kEps);
    TEST_NEAR(label->detailWidth, 204.0, kEps);

    label = nestTileLabel(fx.tree, kSettings, folder, {.x = 0, .y = 0, .w = 166, .h = 50});
    TEST_CHECK(label.has_value() && label->detail == "87.9\u00A0KiB" && label->fontSize == 12 && label->twoLines);

    const auto file = fx.find("f0.dat");
    label = nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 140, .h = 40});
    TEST_CHECK(label.has_value() && label->detail == "1 byte" && label->fontSize == 12 && !label->twoLines);
    TEST_CHECK(label->iconSize == 14);
    TEST_NEAR(label->icon.x, 8.0, kEps);
    TEST_NEAR(label->icon.y, 13.0, kEps);
    TEST_NEAR(label->nameAt.x, 28.0, kEps);
    TEST_NEAR(label->nameAt.y, 24.32, kEps);
    TEST_NEAR(label->nameWidth, 104.0, kEps);
    TEST_NEAR(label->detailAt.x, 132.0, kEps);
    TEST_NEAR(label->detailWidth, 55.8, kEps);
    TEST_CHECK(nestMeasureShown(*label, 30));
    TEST_NEAR(nestNameRoom(*label, 30), 60.0, kEps);
    TEST_CHECK(!nestMeasureShown(*label, 42));
    TEST_NEAR(nestNameRoom(*label, 42), 104.0, kEps);

    label = nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 140, .h = 52});
    TEST_CHECK(label.has_value() && label->fontSize == 13 && label->twoLines && !nestMeasureShown(*label, 1));
    TEST_NEAR(nestNameRoom(*label, 1), 104.0, kEps);

    label = nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 100, .h = 45.9});
    TEST_CHECK(label.has_value() && !label->twoLines);
    label = nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 100, .h = 46});
    TEST_CHECK(label.has_value() && label->twoLines);
    label = nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 44, .h = 22});
    TEST_CHECK(label.has_value() && label->iconSize == 0 && label->fontSize == 12);
    TEST_NEAR(label->nameAt.x, 8.0, kEps);
    TEST_NEAR(label->nameAt.y, 15.32, kEps);
    TEST_CHECK(!nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 43.9, .h = 30}).has_value());
    TEST_CHECK(!nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = 60, .h = 21.9}).has_value());
    TEST_CHECK(!nestTileLabel(fx.tree, kSettings, file, {.x = 0, .y = 0, .w = kNan, .h = 30}).has_value());
    TEST_CHECK(!nestTileLabel(fx.tree, kSettings, kNoNode, {.x = 0, .y = 0, .w = 100, .h = 30}).has_value());
  }

  void moreLabelsOfferALongAndAShortText() {
    auto label = nestMoreLabel(
        392, 952084, MeasureState::Done, {.x = 214.026415, .y = 238.905997, .w = 68.973585, .h = 84.094003}
    );
    TEST_CHECK(label.text == "+392 more" && label.shortText == "+392" && label.size == "929.8\u00A0KiB");
    TEST_NEAR(label.maxWidth, 60.973585, kEps);
    TEST_NEAR(label.center.x, 248.5132075, kEps);
    TEST_NEAR(label.center.y, 272.9529985, kEps);
    label = nestMoreLabel(12345, 10, MeasureState::Done, {.x = 57, .y = 137, .w = 126, .h = 26});
    TEST_CHECK(label.text == "+12,345 more" && label.shortText == "+12,345" && label.size.empty());
    TEST_NEAR(label.center.x, 120.0, kEps);
    TEST_NEAR(label.center.y, 150.0, kEps);
  }

  void fittedTextKeepsThreeCharactersOnEachSideOrNothing() {
    TEST_CHECK(fitNestText("abc", 30, fixedWidth) == "abc");
    TEST_CHECK(fitNestText("abcdefghij", 50, fixedWidth) == "abc…hij");
    TEST_CHECK(fitNestText("abcdefghij", 56, fixedWidth) == "abcd…hij");
    TEST_CHECK(fitNestText("abcdefghij", 48.9, fixedWidth).empty());
    TEST_CHECK(fitNestText("abcdefghij", 28, fixedWidth).empty());
    TEST_CHECK(fitNestText("abc", 17.9, fixedWidth).empty());
    TEST_CHECK(fitNestText("abcdefghij", kNan, fixedWidth).empty());
    TEST_CHECK(fitNestText("", 50, fixedWidth).empty());
    TEST_CHECK(fitNestText("abc", 20, fixedWidth).empty());
    TEST_CHECK(fitNestText("abcd", 25, fixedWidth).empty());
    TEST_CHECK(fitNestText("ééééééééé", 49, fixedWidth) == "ééé…ééé");
    TEST_CHECK(fitNestText("ééééééééé", 48, fixedWidth).empty());
    // The extension stays whole after the ellipsis; with fewer than three characters before it, nothing shows.
    TEST_CHECK(fitNestText("gradient.png", 56, fixedWidth) == "gra….png");
    TEST_CHECK(fitNestText("gradient-map.png", 70, fixedWidth) == "gradi….png");
    TEST_CHECK(fitNestText("gradient.png", 55, fixedWidth).empty());
    TEST_CHECK(fitNestText("data-table.csv", 49, fixedWidth).empty());
    TEST_CHECK(fitNestText("a-very-long-column.txt", 70, fixedWidth) == "a-ver….txt");
    TEST_CHECK(fitNestText("a-very-long-column.txt", 112, fixedWidth) == "a-very-l…umn.txt");
    // A long extension is not kept whole: the text is cut evenly.
    TEST_CHECK(fitNestText("notes.backup-of-old-files", 63, fixedWidth) == "note…iles");
    // A leading stray continuation byte counts as a code point of its own; this measure gives it no width.
    TEST_CHECK(
        fitNestText(
            "\x80"
            "abcdefgh",
            49, fixedWidth
        )
        == "\x80"
           "abc…fgh"
    );
    std::size_t calls = 0;
    const auto counting = [&calls](std::string_view text) {
      ++calls;
      return fixedWidth(text);
    };
    const std::string longText(100000, 'x');
    TEST_CHECK(fitNestText(longText, 70, counting) == "xxxxx…xxxx");
    TEST_CHECK(calls <= 20);
  }

  // A tooltip names the entry on its first line and gives its facts on the second, as the hover pill does.
  void tooltipsDescribeTheEntry() {
    auto fx = fixture();
    TEST_CHECK(
        entryText(entryFacts(fx.tree, fx.find("l3")))
        == "l3\n87.9\u00A0KiB  ·  7 items  ·  0 files  ·  modified 1 h ago"
    );
    TEST_CHECK(entryText(entryFacts(fx.tree, fx.find("f0.dat"))) == "f0.dat\n1 byte  ·  DAT  ·  modified 1 h ago");
    auto& node = fx.tree.nodes[fx.find("s0_0")];
    node.name = "README";
    node.visits = 3;
    node.hidden = true;
    TEST_CHECK(
        entryText(entryFacts(fx.tree, fx.find("README")))
        == "README\n2.0\u00A0KiB  ·  file  ·  modified 1 h ago  ·  3 opens  ·  hidden"
    );
    auto& folder = fx.tree.nodes[fx.find("s0_1")];
    folder.unreadable = true;
    folder.otherFilesystem = true;
    folder.truncated = true;
    folder.files = 1;
    TEST_CHECK(
        entryText(entryFacts(fx.tree, fx.find("s0_1")))
        == "s0_1\n2.9\u00A0KiB  ·  2 items  ·  1 file  ·  modified 1 h ago  ·  on another filesystem  ·  not expanded"
           "  ·  unreadable"
    );
    TEST_CHECK(entryText(entryFacts(fx.tree, kNoNode)).empty());
  }
} // namespace

int main() {
  tileLabelsFollowTheSizeRules();
  tooltipsDescribeTheEntry();
  moreLabelsOfferALongAndAShortText();
  fittedTextKeepsThreeCharactersOnEachSideOrNothing();
  std::println("workbench nest labels checks passed");
  return 0;
}
