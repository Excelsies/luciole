#include "test_check.h"
#include "workbench/format.h"
#include "workbench/iris_overlay.h"
#include "workbench/iris_scene.h"
#include "workbench_fixture.h"
#include "workbench_iris_fixture.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::test::iris;
  using luciole::test::referenceTree;

  constexpr double kClose = 1e-9;

  ring::Dial fullDialOf(double width, double height) { return ring::fullDial(width, height).value(); }

  // Without a font every code point is 6 px wide at 11 px.
  void measuresWithAndWithoutAFont() {
    const TextMeasure plain;
    TEST_NEAR(plain.width("abcd", 11), 24, kClose);
    TEST_NEAR(plain.width("abcd", 22), 48, kClose);
    TEST_NEAR(plain.width("é", 11), 6, kClose);
    TextMeasure font;
    font.font = LabelFont{};
    font.font->ascii['a'] = 5;
    font.font->other = 12;
    TEST_NEAR(font.width("aé", 24), 34, kClose);
    TEST_NEAR(font.width("", 24), 0, kClose);
  }

  void fitCutsInTheMiddle() {
    const TextMeasure plain;
    TEST_CHECK(plain.fit("abcdefghij", 60, 11) == "abcdefghij");
    TEST_CHECK(plain.fit("abcdefghij", 40, 11) == "abc…ij");
    TEST_CHECK(plain.fit("abcdefghij", 5, 11) == "…");
    TEST_CHECK(plain.fit("abcdefghij", 0, 11).empty());
    TEST_CHECK(plain.fit("abcdefghij", kNan, 11).empty());
    TEST_CHECK(plain.fit("abcdefghij", 40, 0).empty());
    TEST_CHECK(plain.fit("", 40, 11).empty());
  }

  // The reference fixture at x (500 bytes, 2 items) on a 1400 by 737 dial: hub radius 113 at (700, 409), 13 px text.
  void hubLinesStackOnTheDialCenter() {
    const auto tree = folderX();
    const auto dial = fullDialOf(1400, 737);
    const std::vector<NodeId> trail{7};
    const auto hub = hubLayout(tree, dial, 5, trail, {}, TextMeasure{});
    TEST_CHECK(hub.lines.size() == 3);
    TEST_CHECK(hub.lines[0].ink == HubInk::Name && hub.lines[0].text == "x");
    // The size is written by the shared formatter.
    TEST_CHECK(hub.lines[1].ink == HubInk::Total && hub.lines[1].text == sizeText(500));
    TEST_CHECK(hub.lines[2].ink == HubInk::Items && hub.lines[2].text == "2 items");
    TEST_NEAR(hub.lines[0].fontSize, 13, kClose);
    TEST_NEAR(hub.lines[1].fontSize, 18.85, kClose);
    TEST_NEAR(hub.lines[2].fontSize, 11.05, kClose);
    // 15.6 + 1 + 22.62 + 1 + 13.26 + 1 + 6 + 17 = 77.48 px tall, centered on y 409.
    TEST_NEAR(hub.lines[0].box.y, 370.26, 1e-9);
    TEST_NEAR(hub.lines[0].box.height, 15.6, 1e-9);
    TEST_NEAR(hub.lines[1].box.y, 386.86, 1e-9);
    TEST_NEAR(hub.lines[2].box.y, 410.48, 1e-9);
    const auto totalChars = static_cast<double>(codePointCount(hub.lines[1].text));
    TEST_NEAR(hub.lines[1].box.width, totalChars * 6 * 18.85 / 11 * 1.1, 1e-9);
    for (const auto& line : hub.lines) {
      TEST_NEAR(line.box.x + line.box.width / 2, 700, 1e-9);
    }
    TEST_CHECK(hub.chips.size() == 1);
    const auto& chip = hub.chips[0];
    TEST_CHECK(chip.node == 7 && chip.text == "deep" && !chip.gap && chip.index == 0);
    TEST_NEAR(chip.fontSize, 11, kClose);
    TEST_NEAR(chip.box.y, 430.74, 1e-9);
    TEST_NEAR(chip.box.x, 679, 1e-9);
    TEST_NEAR(chip.box.width, 42, 1e-9);
    TEST_NEAR(chip.box.height, 17, kClose);
  }

  // On a 900 by 600 dial the hub is 34 px: a total too wide for it shrinks, never below the name's 13 px, then goes.
  void wideTotalsShrinkThenCut() {
    const auto tree = folderX();
    const auto dial = fullDialOf(900, 600);
    TEST_CHECK(dial.rHub == 34);
    // 5.5 px a code point at 11 px; the semibold total is a tenth wider.
    TextMeasure narrow;
    narrow.advance = 5.5;
    const auto hub = hubLayout(tree, dial, 5, {}, {}, narrow);
    const auto& total = hub.lines[1];
    TEST_CHECK(total.text == sizeText(500));
    TEST_CHECK(total.fontSize < 18.85 && total.fontSize >= 13);
    TEST_NEAR(total.box.width, 68, 1e-5);
    TextMeasure wide;
    wide.advance = 20;
    const auto gone = hubLayout(tree, dial, 5, {}, {}, wide);
    TEST_CHECK(std::ranges::none_of(gone.lines, [](const HubLine& line) { return line.ink == HubInk::Total; }));
  }

  // A long trail shows its first two folders and its last after an ellipsis.
  void longTrailsSkipTheMiddle() {
    auto tree = referenceTree();
    const auto dial = fullDialOf(1400, 737);
    const std::vector<NodeId> trail{1, 5, 7, 9};
    const auto hub = hubLayout(tree, dial, 0, trail, {}, TextMeasure{});
    TEST_CHECK(hub.chips.size() == 3);
    TEST_CHECK(hub.chips[0].text == "a" && hub.chips[1].text == "x");
    TEST_CHECK(hub.chips[2].text == "… g.c" && hub.chips[2].gap && hub.chips[2].node == 9);
    TEST_CHECK(hub.chips[1].box.y - hub.chips[0].box.y == kChipHeight + 3);
    // Unknown ids are left out.
    const std::vector<NodeId> unknown{kNoNode, 4000};
    TEST_CHECK(hubLayout(tree, dial, 0, unknown, {}, TextMeasure{}).chips.empty());
  }

  // A name longer than the hub is cut to its width; a chip never gets wider than the hub's body.
  void longNamesAreFitted() {
    auto tree = referenceTree();
    tree.nodes[0].name = std::string(80, 'n');
    tree.nodes[1].name = std::string(80, 'c');
    const auto dial = fullDialOf(1400, 737);
    const std::vector<NodeId> trail{1};
    const auto hub = hubLayout(tree, dial, 0, trail, {}, TextMeasure{});
    TEST_CHECK(hub.lines[0].text.size() < tree.nodes[0].name.size());
    TEST_CHECK(hub.lines[0].box.width <= hub.nameWidth);
    TEST_CHECK(hub.chips[0].box.width <= 2 * hub.radius * 0.82 + kClose);
    TEST_CHECK(hub.chips[0].box.width - 2 * kChipPad <= hub.chipWidth + kClose);
  }

  std::vector<HubLine> noteLines(const Hub& hub) {
    std::vector<HubLine> lines;
    for (const auto& line : hub.lines) {
      if (line.ink == HubInk::Note) {
        lines.push_back(line);
      }
    }
    return lines;
  }

  // A short note is one more line of the hub, 4 px below the lines above it.
  void shortNotesTakeALine() {
    const auto tree = folderX();
    const auto dial = fullDialOf(1400, 737);
    const auto hub = hubLayout(tree, dial, 5, {}, "empty", TextMeasure{});
    TEST_CHECK(hub.lines.size() == 4 && hub.lines[3].ink == HubInk::Note && hub.lines[3].text == "empty");
    TEST_NEAR(hub.lines[3].fontSize, 11.05, 1e-9);
    TEST_NEAR(hub.lines[3].box.width, 5 * 6 * 11.05 / 11, 1e-9);
    TEST_NEAR(hub.lines[3].box.y - (hub.lines[2].box.y + hub.lines[2].box.height), 5, 1e-9);
  }

  // A note wider than the hub's body wraps at its spaces onto lines of the hub, 1 px apart, each within the body.
  // The hub is 113 px, so its body is 185.32 px: 30 characters of 11.05 px text.
  void longNotesWrapInTheHub() {
    const auto tree = folderX();
    const auto dial = fullDialOf(1400, 737);
    const std::string note = "Only 1,234 hidden entries (Ctrl+H shows them)";
    const auto hub = hubLayout(tree, dial, 5, {}, note, TextMeasure{});
    const auto lines = noteLines(hub);
    TEST_CHECK(lines.size() == 2);
    TEST_CHECK(lines[0].text == "Only 1,234 hidden entries" && lines[1].text == "(Ctrl+H shows them)");
    TEST_NEAR(lines[1].box.y - (lines[0].box.y + lines[0].box.height), 1, 1e-9);
    for (const auto& line : lines) {
      TEST_NEAR(line.box.x + line.box.width / 2, 700, 1e-9);
      TEST_CHECK(line.box.width <= 185.32 + kClose);
      TEST_NEAR(line.box.width, TextMeasure{}.width(line.text, line.fontSize), 1e-9);
    }
    // The body, the note's two lines included, is centered on the hub.
    const auto& last = hub.lines.back();
    TEST_NEAR((hub.lines.front().box.y + last.box.y + last.box.height) / 2, 409, 1e-9);
    // A word wider than the body is cut in the middle to fit it.
    const std::string word(200, 'w');
    const auto cut = noteLines(hubLayout(tree, dial, 5, {}, word, TextMeasure{}));
    TEST_CHECK(cut.size() == 1 && cut[0].text.contains("…"));
    TEST_CHECK(cut[0].box.width <= 185.32 + kClose);
  }

  // In small panes the wrapped note stays inside the main ring: no end of a line reaches its outer edge.
  void notesStayInsideSmallDials() {
    const auto tree = folderX();
    const std::string note = "Only 1,234 hidden entries (Ctrl+H shows them)";
    for (const auto& [width, height] : {std::pair{340.0, 534.0}, {300.0, 330.0}, {260.0, 240.0}}) {
      const auto dial = ring::compactDial(width, height, 1).value();
      const auto hub = hubLayout(tree, dial, 5, {}, note, TextMeasure{});
      const auto lines = noteLines(hub);
      TEST_CHECK(!lines.empty());
      std::string joined;
      for (const auto& line : lines) {
        joined += (joined.empty() ? "" : " ") + line.text;
        const double halfWidth = line.box.width / 2;
        for (const double y : {line.box.y, line.box.y + line.box.height}) {
          TEST_CHECK(std::hypot(halfWidth, y - dial.center.y) < dial.rOut);
        }
      }
      // Whole, or ending in an ellipsis where the hub has no room for all of it.
      TEST_CHECK(joined == note || (joined.ends_with("…") && note.starts_with(joined.substr(0, joined.size() - 3))));
    }
  }

  // Where its wrapped lines would leave the hub's disc, the note shrinks until each line lies inside it, still whole;
  // a note that fits once wrapped keeps its size.
  void notesShrinkOnlyWhenWrappingLeavesTheHub() {
    const auto tree = folderX();
    const std::string note = "Only 1,234 hidden entries (Ctrl+H shows them)";
    const auto roomy = noteLines(hubLayout(tree, fullDialOf(1400, 737), 5, {}, note, TextMeasure{}));
    TEST_CHECK(!roomy.empty());
    TEST_NEAR(roomy[0].fontSize, 11.05, 1e-9);
    // A hub of 49 px: at 10.2 px the last of the three lines reaches past its edge.
    const auto dial = ring::compactDial(280, 300, 1).value();
    const auto hub = hubLayout(tree, dial, 5, {}, note, TextMeasure{});
    TEST_NEAR(hub.radius, 49, 1e-9);
    const auto lines = noteLines(hub);
    TEST_CHECK(!lines.empty());
    TEST_CHECK(lines[0].fontSize < hub.fontSize * 0.85 && lines[0].fontSize >= 8);
    std::string joined;
    for (const auto& line : lines) {
      joined += (joined.empty() ? "" : " ") + line.text;
      TEST_NEAR(line.fontSize, lines[0].fontSize, 1e-9);
      for (const double y : {line.box.y, line.box.y + line.box.height}) {
        TEST_CHECK(std::hypot(line.box.width / 2, y - dial.center.y) <= hub.radius + 1e-6);
      }
    }
    TEST_CHECK(joined == note);
  }

  void smallHubsDropChipsThenEverything() {
    const auto tree = folderX();
    const std::vector<NodeId> trail{7};
    // Compact at 340 by 534 with two rings: hub radius 57, room for chips.
    const auto roomy = ring::compactDial(340, 534, 2).value();
    const auto withChips = hubLayout(tree, roomy, 5, trail, {}, TextMeasure{});
    TEST_CHECK(withChips.radius == 57 && withChips.chips.size() == 1 && withChips.lines.size() == 3);
    TEST_NEAR(withChips.lines[1].fontSize, 15, kClose);
    TEST_NEAR(withChips.lines[2].fontSize, 11, kClose);
    // Compact at 240 by 220: no stack, a hub of 25 px is tiny.
    const auto tiny = ring::compactDial(240, 220, 2).value();
    const auto none = hubLayout(tree, tiny, 5, trail, {}, TextMeasure{});
    TEST_CHECK(none.tiny && none.lines.empty() && none.chips.empty());
    // Compact at 300 by 330: a hub of 42 px keeps its items and drops the trail.
    const auto small = ring::compactDial(300, 330, 2).value();
    const auto noChips = hubLayout(tree, small, 5, trail, {}, TextMeasure{});
    TEST_CHECK(!noChips.tiny && noChips.chips.empty() && noChips.lines.size() == 3);
  }

  // Each row's background is its text and 6 px each side, centered on the stack.
  void stackRowsAreAsWideAsTheirText() {
    const auto tree = folderX();
    const auto dial = fullDialOf(1400, 737);
    const auto stack = ancestorStack(tree, dial, TextMeasure{});
    TEST_CHECK(stack.rows.size() == 2);
    TEST_CHECK(stack.rows[0].text == "r" && stack.rows[1].text == "a");
    for (const auto& row : stack.rows) {
      TEST_NEAR(row.box.width, 6.0 * 12 / 11 + 2 * kStackPad, 1e-9);
      TEST_NEAR(row.box.x + row.box.width / 2, 700, 1e-9);
      TEST_NEAR(row.box.height, 15, kClose);
    }
    TEST_NEAR(stack.rows[0].box.y, 55, kClose);
    TEST_NEAR(stack.rows[1].box.y, 70, kClose);
  }

  // A long name is cut to the stack's text width.
  void stackNamesAreFitted() {
    auto tree = folderX();
    tree.nodes[1].name = std::string(100, 'q');
    const auto stack = ancestorStack(tree, fullDialOf(1400, 737), TextMeasure{});
    const auto& row = stack.rows.back();
    TEST_CHECK(row.text != tree.nodes[1].name);
    TEST_CHECK(row.box.width <= stack.textWidth + 2 * kStackPad + kClose);
    TEST_CHECK(row.box.width > stack.textWidth - 10);
  }

  void readoutSitsAtTheBottomOfACompactDial() {
    const auto compact = readout(ring::compactDial(340, 534, 2).value());
    TEST_CHECK(compact.has_value());
    TEST_CHECK((compact->box == Rect{.x = 6, .y = 512, .width = 328, .height = 18}));
    TEST_NEAR(compact->fontSize, 11, kClose);
    TEST_CHECK(!readout(fullDialOf(1400, 737)).has_value());
  }

  // buildScene lays the hub and the stack out with its measure and the trail it is given.
  void scenesCarryTheOverlay() {
    const auto tree = folderX();
    auto input = hostInput(340, 534, true);
    input.trail = {7};
    const auto scene = buildScene(tree, input).value();
    TEST_CHECK(scene.hub.chips.size() == 1 && scene.hub.chips[0].node == 7);
    TEST_CHECK(scene.hub.lines.size() == 3);
    TEST_CHECK(scene.readout.has_value());
    TEST_CHECK(scene.stack.rows.size() == 2);
    TEST_CHECK(scene.settings == input.settings);
    const auto full = buildScene(tree, fullInput()).value();
    TEST_CHECK(!full.readout.has_value() && full.hub.chips.empty());
  }

  // A partial folder with nothing to show is still loading.
  void partialEmptyFoldersAreLoading() {
    auto tree = folderX();
    tree.nodes[8].hidden = true;
    tree.nodes[7].hidden = true;
    tree.nodes[5].partial = true;
    TEST_CHECK(hubNote(tree, 5, 0, {}) == "Loading…");
    tree.nodes[5].partial = false;
    TEST_CHECK(hubNote(tree, 5, 0, {}) == "Only 2 hidden entries (Ctrl+H shows them)");
  }

  // The note keeps 2 px clear of the hub's border, however small the hub; where it cannot, the hub gives up its size
  // and count lines, then the note its last lines.
  void notesKeepClearOfTheHubsBorder() {
    const auto tree = folderX();
    const std::string note = "Not expanded in this snapshot: 1,234 items";
    std::size_t checked = 0;
    for (const double width : {240.0, 260.0, 280.0, 300.0, 340.0}) {
      for (const double height : {260.0, 300.0, 330.0, 420.0, 534.0}) {
        for (const std::size_t rings : {1U, 2U, 3U}) {
          const auto dial = ring::compactDial(width, height, rings);
          if (!dial) {
            continue;
          }
          const auto hub = hubLayout(tree, *dial, 5, {}, note, TextMeasure{});
          if (hub.tiny) {
            continue;
          }
          ++checked;
          for (const auto& line : noteLines(hub)) {
            for (const double y : {line.box.y, line.box.y + line.box.height}) {
              TEST_CHECK(std::hypot(line.box.width / 2, y - dial->center.y) <= hub.radius - kNoteClearance + 1e-6);
            }
          }
        }
      }
    }
    TEST_CHECK(checked >= 10);
  }

  // A note that cannot be shown whole in a small hub ends in an ellipsis rather than reaching the border.
  void aNoteThatCannotFitIsCutWithAnEllipsis() {
    const auto tree = folderX();
    const std::string note = "Not expanded in this snapshot: 1,234 items and a great deal more words after them";
    const auto dial = ring::compactDial(300, 330, 1).value();
    const auto lines = noteLines(hubLayout(tree, dial, 5, {}, note, TextMeasure{}));
    TEST_CHECK(lines.size() == 3);
    TEST_CHECK(lines.back().text.ends_with("…"));
  }

  // A size and a count are shown whole or not at all.
  void numbersAreNeverShortened() {
    const auto tree = folderX();
    for (const double advance : {6.0, 12.0, 20.0, 40.0}) {
      TextMeasure measure;
      measure.advance = advance;
      for (const auto& dial : {fullDialOf(900, 600), ring::compactDial(300, 330, 2).value(), fullDialOf(1400, 737)}) {
        const auto hub = hubLayout(tree, dial, 5, {}, {}, measure);
        for (const auto& line : hub.lines) {
          TEST_CHECK(line.ink != HubInk::Total || line.text == sizeText(500));
          TEST_CHECK(line.ink != HubInk::Items || line.text == "2 items");
          if (line.ink == HubInk::Total || line.ink == HubInk::Items) {
            TEST_CHECK(line.box.width <= 2 * hub.radius + 1e-5);
          }
        }
      }
    }
    // A hub too small for the count's width leaves the line out.
    TextMeasure wide;
    wide.advance = 40;
    const auto hub = hubLayout(tree, fullDialOf(900, 600), 5, {}, {}, wide);
    TEST_CHECK(std::ranges::none_of(hub.lines, [](const HubLine& line) { return line.ink == HubInk::Items; }));
  }

  // The name is semibold, wider than regular: it is measured so, and fits the room as drawn.
  void theNameIsMeasuredSemibold() {
    auto tree = folderX();
    tree.nodes[5].name = std::string(40, 'n');
    const auto dial = fullDialOf(900, 600);
    // Without a semibold font: 6 px a code point at 11 px, a tenth wider semibold; a 13 px name has 51 px.
    const TextMeasure plain;
    const auto hub = hubLayout(tree, dial, 5, {}, {}, plain);
    const auto& name = hub.lines[0];
    TEST_CHECK(name.ink == HubInk::Name && name.text != tree.nodes[5].name);
    TEST_NEAR(hub.nameWidth, 51, kClose);
    TEST_NEAR(name.box.width, plain.width(name.text, 13) * 1.1, 1e-9);
    TEST_CHECK(name.box.width <= 51 + 1e-9);
    TEST_CHECK(codePointCount(name.text) == 6);
    // With a semibold font of 8 px a code point against 6 regular, fewer code points fit.
    TextMeasure measured;
    measured.font = LabelFont{};
    measured.font->ascii.fill(6);
    measured.font->other = 6;
    measured.boldFont = LabelFont{};
    measured.boldFont->ascii.fill(9);
    measured.boldFont->other = 9;
    const auto real = hubLayout(tree, dial, 5, {}, {}, measured);
    TEST_CHECK(real.lines[0].box.width <= real.nameWidth + 1e-9);
    TEST_NEAR(real.lines[0].box.width, codePointCount(real.lines[0].text) * 9 * 13.0 / 12.0, 1e-9);
    TEST_CHECK(
        codePointCount(real.lines[0].text) < codePointCount(hubLayout(
                                                                tree, dial, 5, {}, {},
                                                                [&] {
                                                                  auto regular = measured;
                                                                  regular.boldFont.reset();
                                                                  return regular;
                                                                }()
        )
                                                                .lines[0]
                                                                .text)
    );
  }

  // The folded row says how many levels it stands for; a long name after it is cut, never the count.
  void theFoldedLevelsCountIsNeverCut() {
    std::vector<luciole::test::WorkbenchRow> rows{
        {.parent = -1, .name = std::string_view(), .folder = true, .items = 1}
    };
    std::vector<std::string> names;
    names.reserve(16);
    for (int i = 0; i < 14; ++i) {
      names.push_back(i == 0 ? std::string(100, 'q') : "level" + std::to_string(i));
    }
    for (int i = 0; i < 14; ++i) {
      rows.push_back({.parent = i, .name = names[static_cast<std::size_t>(i)], .folder = true, .items = 1});
    }
    auto tree = luciole::test::workbenchTree(rows, 0);
    tree.root = 14;
    tree.ancestors = parentChain(tree, tree.root);
    const auto dial = fullDialOf(1400, 737);
    // 9 px a code point at 11 px: the text room holds 28 of them, the count and its words 19.
    TextMeasure measure;
    measure.advance = 9;
    const auto stack = ancestorStack(tree, dial, measure);
    TEST_CHECK(stack.rows.size() == 5);
    const auto& folded = stack.rows.front();
    const auto prefix = std::format("⋯ {} more levels · ", tree.ancestors.size() - 4);
    TEST_CHECK(folded.text.starts_with(prefix));
    TEST_CHECK(folded.text != prefix + names[0] && folded.text.size() > prefix.size());
    TEST_CHECK(folded.box.width <= stack.textWidth + 2 * kStackPad + kClose);
    // 14 px: the count fits without the name, which is left out.
    TextMeasure wide;
    wide.advance = 14;
    TEST_CHECK(
        ancestorStack(tree, dial, wide).rows.front().text
        == prefix.substr(0, prefix.size() - std::string_view(" · ").size())
    );
    // 18 px: not even the count and its words fit, so the row has no text rather than a cut count.
    wide.advance = 18;
    const auto tight = ancestorStack(tree, dial, wide);
    TEST_CHECK(tight.rows.front().text.empty());
  }
} // namespace

int main() {
  measuresWithAndWithoutAFont();
  fitCutsInTheMiddle();
  hubLinesStackOnTheDialCenter();
  wideTotalsShrinkThenCut();
  longTrailsSkipTheMiddle();
  longNamesAreFitted();
  shortNotesTakeALine();
  longNotesWrapInTheHub();
  notesStayInsideSmallDials();
  notesShrinkOnlyWhenWrappingLeavesTheHub();
  notesKeepClearOfTheHubsBorder();
  aNoteThatCannotFitIsCutWithAnEllipsis();
  numbersAreNeverShortened();
  theNameIsMeasuredSemibold();
  smallHubsDropChipsThenEverything();
  stackRowsAreAsWideAsTheirText();
  stackNamesAreFitted();
  theFoldedLevelsCountIsNeverCut();
  readoutSitsAtTheBottomOfACompactDial();
  scenesCarryTheOverlay();
  partialEmptyFoldersAreLoading();
  return 0;
}
