#include "test_check.h"
#include "ui/controls/shape_labels.h"
#include "workbench/scene_shapes.h"

#include <cstddef>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Labels fitted to their boxes: names cut in the middle or left out, counts and measures whole or left out, and
// trails shown only with their labels.
namespace {
  using namespace luciole::workbench;

  constexpr double kEps = 1e-4;
  constexpr Rgba8 kInk{200, 200, 200, 255};
  constexpr Rgba8 kTrailInk{150, 150, 150, 255};

  // Half the font size per code point; semibold one px more.
  double measure(std::string_view text, double size, LabelWeight weight) {
    double count = 0;
    for (const char c : text) {
      if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
        ++count;
      }
    }
    return count * (size / 2 + (weight == LabelWeight::Semibold ? 1 : 0));
  }

  ShapeLabel label(std::string text, float width, float size = 12) {
    return {
        .node = 7,
        .text = std::move(text),
        .x = 10,
        .y = 20,
        .width = width,
        .height = size * 1.3F,
        .align = LabelAlign::Start,
        .fontSize = size,
        .rotation = 0,
        .color = kInk,
    };
  }

  std::vector<ShapeLabel> fitted(std::vector<ShapeLabel> labels) {
    fitShapeLabels(labels, measure);
    return labels;
  }

  void namesAreCutInTheMiddleOrLeftOut() {
    auto out = fitted({label("a-very-long-column.txt", 120), label("data.csv", 40), label("notes.txt", 60)});
    TEST_CHECK(out.size() == 2);
    TEST_CHECK(out[0].text == "a-very-lon…olumn.txt");
    TEST_NEAR(out[0].width, 120.0, kEps);
    TEST_CHECK(out[1].text == "notes.txt");
    out = fitted({label("", 100), label("abcdef", std::numeric_limits<float>::quiet_NaN())});
    TEST_CHECK(out.empty());
  }

  void countsAndMeasuresAreWholeOrLeftOut() {
    auto count = label("+10 more", 40, 13);
    count.fit = LabelFit::Whole;
    count.shortText = "+10";
    auto size = label("3.9 KiB", 30, 11);
    size.fit = LabelFit::Whole;
    auto whole = label("929.8 KiB", 60, 11);
    whole.fit = LabelFit::Whole;
    auto out = fitted({count, size, whole});
    TEST_CHECK(out.size() == 2);
    TEST_CHECK(out[0].text == "+10");
    TEST_CHECK(out[1].text == "929.8 KiB");
    count.width = 52;
    TEST_CHECK(fitted({count}).front().text == "+10 more");
    count.width = 15;
    TEST_CHECK(fitted({count}).empty());
  }

  void semiboldTextIsMeasuredSemibold() {
    auto name = label("abcdef", 40);
    TEST_CHECK(fitted({name}).front().text == "abcdef");
    name.weight = LabelWeight::Semibold;
    TEST_CHECK(fitted({name}).empty());
  }

  ShapeLabel withEndTrail(std::string text, float width, float room) {
    auto name = label(std::move(text), width);
    name.trail = LabelTrail{
        .text = "67 bytes", .place = TrailPlace::End, .fontSize = 11, .color = kTrailInk, .gap = 14, .room = room
    };
    return name;
  }

  // A measure at the end of the line takes its width and gap from the name's room when both take less than the
  // trail's room.
  void endTrailsShareTheLine() {
    auto out = fitted({withEndTrail("README.md", 100, 60), label("next", 50)});
    TEST_CHECK(out.size() == 3);
    TEST_CHECK(out[0].text == "REA….md");
    TEST_NEAR(out[0].width, 42.0, kEps);
    TEST_CHECK(!out[0].trail.has_value());
    const auto& trail = out[1];
    TEST_CHECK(trail.text == "67 bytes" && trail.node == 7 && trail.fit == LabelFit::Whole);
    TEST_CHECK(trail.align == LabelAlign::End && trail.fontSize == 11 && trail.color == kTrailInk);
    TEST_CHECK(trail.weight == LabelWeight::Regular && !trail.trail.has_value());
    TEST_NEAR(trail.x, 10.0, kEps);
    TEST_NEAR(trail.width, 100.0, kEps);
    TEST_NEAR(trail.height, 14.3, kEps);
    TEST_NEAR(trail.y + trail.height / 2, 20 + 7.8, kEps);
    TEST_CHECK(out[2].text == "next");
    // At 58 of room the measure and its gap do not fit: the name keeps the whole line.
    out = fitted({withEndTrail("README.md", 100, 58)});
    TEST_CHECK(out.size() == 1 && out[0].text == "README.md");
    TEST_NEAR(out[0].width, 100.0, kEps);
  }

  // The measure goes first: a name that cannot show beside it shows alone.
  void endTrailsGiveWayToTheName() {
    auto name = withEndTrail("abcdefgh", 50, 60);
    name.trail->text = "12";
    const auto out = fitted({name});
    TEST_CHECK(out.size() == 1 && out[0].text == "abcdefgh");
    TEST_NEAR(out[0].width, 50.0, kEps);
    name.width = 40;
    TEST_CHECK(fitted({name}).empty());
  }

  ShapeLabel countWithSize(float width, float sizeWidth) {
    auto count = label("+2 more", width, 13);
    count.align = LabelAlign::Center;
    count.fit = LabelFit::Whole;
    count.shortText = "+2";
    count.weight = LabelWeight::Semibold;
    count.trail = LabelTrail{
        .text = "11.4 KiB",
        .place = TrailPlace::Below,
        .fontSize = 11,
        .color = kTrailInk,
        .dx = 0,
        .dy = 18,
        .width = sizeWidth,
    };
    return count;
  }

  // A line under a label shows only with it, whole: the size line goes before the count.
  void belowTrailsFollowTheirLabel() {
    auto out = fitted({countWithSize(100, 100)});
    TEST_CHECK(out.size() == 2);
    TEST_CHECK(out[0].text == "+2 more");
    const auto& size = out[1];
    TEST_CHECK(size.text == "11.4 KiB" && size.align == LabelAlign::Center && size.fit == LabelFit::Whole);
    TEST_NEAR(size.x, 10.0, kEps);
    TEST_NEAR(size.width, 100.0, kEps);
    TEST_NEAR(size.height, 14.3, kEps);
    TEST_NEAR(size.y + size.height / 2, 20 + 8.45 + 18, kEps);
    out = fitted({countWithSize(40, 40)});
    TEST_CHECK(out.size() == 1 && out[0].text == "+2");
    // A count down to its short form shows no size line, though the line has room.
    out = fitted({countWithSize(45, 100)});
    TEST_CHECK(out.size() == 1 && out[0].text == "+2");
    out = fitted({countWithSize(10, 100)});
    TEST_CHECK(out.empty());
  }
} // namespace

int main() {
  namesAreCutInTheMiddleOrLeftOut();
  countsAndMeasuresAreWholeOrLeftOut();
  semiboldTextIsMeasuredSemibold();
  endTrailsShareTheLine();
  endTrailsGiveWayToTheName();
  belowTrailsFollowTheirLabel();
  std::println("shape labels fit checks passed");
  return 0;
}
