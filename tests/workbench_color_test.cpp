#include "test_check.h"
#include "workbench/color.h"
#include "workbench_fixture.h"

#include <cmath>
#include <limits>
#include <print>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using luciole::WorkbenchColor;
  using luciole::test::referenceTree;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr Rgb kOnSurface{0xEC, 0xE9, 0xF4};
  constexpr Rgb kSurface{0x15, 0x16, 0x2E};
  constexpr Rgb kRampBottom{0x2E, 0x2F, 0x58};
  constexpr Rgb kRampTop{0xF7, 0xE9, 0xA8};

  Rgb categoryAccent(FileCategory category) { return ColorPalette{}.categories[static_cast<std::size_t>(category)]; }

  LayoutSettings coloredBy(WorkbenchColor colorBy) {
    LayoutSettings settings;
    settings.colorBy = colorBy;
    return settings;
  }

  void testCategories() {
    TEST_CHECK(fileExtension("b.txt") == "txt");
    TEST_CHECK(fileExtension("archive.TAR.GZ") == "gz");
    TEST_CHECK(fileExtension(".bashrc").empty());
    TEST_CHECK(fileExtension("trailing.").empty());
    TEST_CHECK(fileExtension("noext").empty());
    TEST_CHECK(fileExtension("").empty());
    TEST_CHECK(fileCategory("b.txt", false) == FileCategory::Document);
    TEST_CHECK(fileCategory("y.png", false) == FileCategory::Image);
    TEST_CHECK(fileCategory("g.c", false) == FileCategory::Code);
    TEST_CHECK(fileCategory("a", true) == FileCategory::Folder);
    TEST_CHECK(fileCategory("x.png", true) == FileCategory::Folder);
    TEST_CHECK(fileCategory("Photo.JPG", false) == FileCategory::Image);
    TEST_CHECK(fileCategory("clip.mkv", false) == FileCategory::Video);
    TEST_CHECK(fileCategory("song.flac", false) == FileCategory::Audio);
    TEST_CHECK(fileCategory("pack.zst", false) == FileCategory::Archive);
    TEST_CHECK(fileCategory("rows.csv", false) == FileCategory::Data);
    TEST_CHECK(fileCategory("tool.appimage", false) == FileCategory::Executable);
    TEST_CHECK(fileCategory("libx.a", false) == FileCategory::Executable);
    TEST_CHECK(fileCategory("notes.unknown", false) == FileCategory::Other);
    TEST_CHECK(fileCategory("odd.png jpg", false) == FileCategory::Other);
    TEST_CHECK(fileCategory(".png", false) == FileCategory::Other);
    TEST_CHECK(FileCategory{} == FileCategory::Other);
  }

  void testNormalizeAndRamp() {
    TEST_CHECK(normalize(10, 10, 1000) == 0);
    TEST_CHECK(normalize(1000, 10, 1000) == 1);
    TEST_CHECK(normalize(5, 5, 5) == 0.5);
    TEST_NEAR(normalize(std::sqrt(11.0 * 1001.0) - 1, 10, 1000), 0.5, 1e-9);
    const auto stops = ColorPalette{}.ramp;
    TEST_CHECK(ramp(0, stops) == kRampBottom);
    TEST_CHECK(ramp(1, stops) == kRampTop);
    TEST_CHECK((ramp(1.0 / 3.0, stops) == Rgb{0x5B, 0x60, 0xA8}));
    TEST_CHECK((ramp(0.1, stops) == Rgb{0x3C, 0x3E, 0x70}));
    TEST_CHECK((ramp(0.25, stops) == Rgb{0x50, 0x54, 0x94}));
    TEST_CHECK((ramp(0.5, stops) == Rgb{0x7E, 0x9F, 0xB4}));
    TEST_CHECK((ramp(0.75, stops) == Rgb{0xB6, 0xE0, 0xBA}));
    TEST_CHECK((ramp(0.9, stops) == Rgb{0xDD, 0xE5, 0xAF}));
    TEST_CHECK((mix({0, 0, 0}, {255, 255, 255}, 0.5) == Rgb{0x80, 0x80, 0x80}));
    TEST_CHECK((mix(kRampBottom, {0x5B, 0x60, 0xA8}, 0.5) == Rgb{0x45, 0x48, 0x80}));
  }

  void testLuminanceAndOklab() {
    TEST_CHECK(luminance({255, 255, 255}) == 1);
    TEST_CHECK(luminance({0, 0, 0}) == 0);
    TEST_NEAR(luminance({0x80, 0x80, 0x80}), 0.21586050011389923, 1e-15);
    TEST_NEAR(contrast({255, 255, 255}, {0, 0, 0}), 21.0, 1e-12);
    TEST_NEAR(contrast({0, 0, 0}, {255, 255, 255}), 21.0, 1e-12);
    const auto gold = toOklch(kRampTop);
    TEST_NEAR(gold.l, 0.9300900882707195, 1e-12);
    TEST_NEAR(gold.c, 0.08402786701974417, 1e-12);
    TEST_NEAR(gold.h, 1.7006684428254442, 1e-12);
    TEST_CHECK((fromOklch({.l = 0.5, .c = 0, .h = 0}) == Rgb{0x63, 0x63, 0x63}));
    TEST_CHECK((fromOklch({.l = 1, .c = 0, .h = 0}) == Rgb{255, 255, 255}));
    TEST_CHECK((fromOklch({.l = 0, .c = 0, .h = 0}) == Rgb{0, 0, 0}));
    TEST_CHECK((fromOklch({.l = 0.7, .c = 0.4, .h = 1}) == Rgb{0xE6, 0x80, 0x1E}));
    TEST_CHECK((fromOklch({.l = 0.6, .c = 0.1, .h = 2}) == Rgb{0x7F, 0x87, 0x3C}));
    TEST_CHECK((fromOklch({.l = 0.3, .c = 0.15, .h = 4}) == Rgb{0x00, 0x33, 0x46}));
    const auto back = toOklch(fromOklch({.l = 0.6, .c = 0.1, .h = 2}));
    TEST_NEAR(back.l, 0.6, 0.01);
    TEST_NEAR(back.c, 0.1, 0.01);
  }

  void testCategoryFills() {
    const ColorPalette palette;
    const std::vector<Rgb> expected{
        {0x3C, 0x3E, 0x60}, {0x46, 0x49, 0x74}, {0x25, 0x5D, 0x46}, {0x7E, 0x23, 0x3B}, {0x72, 0x42, 0x1A},
        {0x63, 0x57, 0x1A}, {0x1E, 0x53, 0x76}, {0x64, 0x3B, 0x70}, {0x00, 0x5C, 0x58}, {0x4D, 0x4F, 0x64},
    };
    for (std::size_t i = 0; i < kFileCategoryCount; ++i) {
      const auto fill = fillFor(palette.categories[i]);
      TEST_CHECK(fill == expected[i]);
      TEST_CHECK(inkFor(fill, palette) == kOnSurface);
      for (std::size_t j = 0; j < i; ++j) {
        TEST_CHECK(fill != fillFor(palette.categories[j]));
      }
    }
  }

  void testInkContrast() {
    const ColorPalette palette;
    for (const auto accent : palette.categories) {
      const auto fill = fillFor(accent);
      TEST_CHECK(contrast(fill, inkFor(fill, palette)) >= 4.5);
    }
    for (int i = 0; i <= 10; ++i) {
      const auto fill = fillFor(ramp(i / 10.0, palette.ramp));
      TEST_CHECK(contrast(fill, inkFor(fill, palette)) >= 4.5);
    }
    ColorPalette light = palette;
    light.surface = {0xFF, 0xFF, 0xFF};
    light.onSurface = {0x10, 0x10, 0x10};
    TEST_CHECK((inkFor({0xF0, 0xF0, 0xF0}, light) == Rgb{0x10, 0x10, 0x10}));
    TEST_CHECK((inkFor({0x20, 0x20, 0x20}, light) == Rgb{0xFF, 0xFF, 0xFF}));
  }

  void testFillKeepsHue() {
    const ColorPalette palette;
    std::vector<Rgb> accents(palette.categories.begin(), palette.categories.end());
    accents.insert(accents.end(), palette.ramp.begin(), palette.ramp.end());
    for (const auto accent : accents) {
      const auto a = toOklch(accent);
      const auto f = toOklch(fillFor(accent));
      TEST_CHECK(f.l < a.l || a.l < 0.4);
      TEST_CHECK(f.l >= 0.29 && f.l <= 0.47);
      if (a.c > 0.04) {
        TEST_CHECK(std::abs(std::atan2(std::sin(a.h - f.h), std::cos(a.h - f.h))) < 0.12);
      }
    }
    TEST_CHECK(toOklch(fillFor(categoryAccent(FileCategory::Document))).c > 0.05);
  }

  void testNodeColorByType() {
    const auto tree = referenceTree();
    const LayoutSettings settings;
    const auto folder = nodeColor(tree, 1, settings);
    TEST_CHECK((folder.accent == Rgb{0xB3, 0xB8, 0xED}));
    TEST_CHECK((folder.fill == Rgb{0x46, 0x49, 0x74}));
    TEST_CHECK(folder.ink == kOnSurface || folder.ink == kSurface);
    TEST_CHECK((nodeColor(tree, 2, settings).accent == Rgb{0xF7, 0xE9, 0xA8}));
    ColorPalette custom;
    custom.categories[static_cast<std::size_t>(FileCategory::Folder)] = {0xFF, 0x00, 0x00};
    TEST_CHECK((nodeColor(tree, 1, settings, custom).accent == Rgb{0xFF, 0x00, 0x00}));
  }

  void testNodeColorByMeasure() {
    const auto tree = referenceTree();
    auto settings = coloredBy(WorkbenchColor::Bytes);
    TEST_CHECK(nodeColor(tree, 1, settings).accent == kRampTop);
    TEST_CHECK(nodeColor(tree, 4, settings).accent == kRampBottom);
    TEST_CHECK((nodeColor(tree, 2, settings) == NodeColor{{0x52, 0x59, 0x2C}, kOnSurface, {0xDB, 0xE5, 0xB0}}));
    TEST_CHECK((nodeColor(tree, 0, settings) == NodeColor{{0x2D, 0x4A, 0x5C}, kOnSurface, {0x7E, 0x9F, 0xB4}}));
    settings = coloredBy(WorkbenchColor::Visits);
    TEST_CHECK((nodeColor(tree, 2, settings) == NodeColor{{0x29, 0x57, 0x4D}, kOnSurface, {0x95, 0xC9, 0xBC}}));
    settings = coloredBy(WorkbenchColor::Modified);
    TEST_CHECK(nodeColor(tree, 1, settings).accent == kRampTop);
    TEST_CHECK(nodeColor(tree, 4, settings).accent == kRampBottom);
    TEST_CHECK((nodeColor(tree, 2, settings) == NodeColor{{0x31, 0x31, 0x70}, kOnSurface, {0x59, 0x5D, 0xA4}}));
    const auto range = colorRange(tree, 0, WorkbenchColor::Modified, false);
    TEST_CHECK(range.min == 2000);
    TEST_CHECK(range.max == 5500);
    TEST_CHECK(nodeColor(tree, 2, WorkbenchColor::Modified, range, ColorPalette{}) == nodeColor(tree, 2, settings));
  }

  void testColorFollowsVisits() {
    auto tree = referenceTree();
    const auto settings = coloredBy(WorkbenchColor::Visits);
    const auto before = nodeColor(tree, 2, settings).accent;
    tree.nodes[2].visits = 6;
    TEST_CHECK(nodeColor(tree, 2, settings).accent != before);
    TEST_CHECK(nodeColor(tree, 2, settings).accent == kRampTop);
  }

  void testDegenerateInputs() {
    auto tree = referenceTree();
    const auto stops = ColorPalette{}.ramp;
    TEST_CHECK(normalize(kNan, 1, 10) == 0.5);
    TEST_CHECK(normalize(3, 10, 1) == 0.5);
    TEST_CHECK(normalize(3, kNan, 10) == 0.5);
    TEST_CHECK(normalize(100, 1, 10) == 1);
    TEST_CHECK(normalize(-5, 1, 10) == 0.5);
    TEST_CHECK(ramp(kNan, stops) == kRampBottom);
    TEST_CHECK(ramp(-1, stops) == kRampBottom);
    TEST_CHECK(ramp(5, stops) == kRampTop);
    TEST_CHECK((fromOklch({.l = kNan, .c = kNan, .h = kNan}) == Rgb{0, 0, 0}));
    TEST_CHECK((fromOklch({.l = 5, .c = 3, .h = 1}) == Rgb{255, 255, 255}));
    TEST_CHECK((mix(kRampBottom, kRampTop, kNan) == Rgb{0, 0, 0}));
    TEST_CHECK(nodeColor(tree, 99, LayoutSettings{}) == NodeColor{});
    const auto empty = colorRange(tree, 4, WorkbenchColor::Bytes, false);
    TEST_CHECK(empty.min == 0 && empty.max == 0);
    TEST_CHECK(colorRange(tree, kNoNode, WorkbenchColor::Bytes, false).max == 0);
    tree.nodes[2].category = 200;
    TEST_CHECK(nodeColor(tree, 2, LayoutSettings{}).accent == categoryAccent(FileCategory::Other));
    tree.nodes[1].bytes = kNan;
    const auto settings = coloredBy(WorkbenchColor::Bytes);
    TEST_CHECK(nodeColor(tree, 1, settings).accent == kRampBottom);
    TEST_CHECK(nodeColor(tree, 2, settings).accent == kRampTop);
  }
} // namespace

int main() {
  testCategories();
  testNormalizeAndRamp();
  testLuminanceAndOklab();
  testCategoryFills();
  testInkContrast();
  testFillKeepsHue();
  testNodeColorByType();
  testNodeColorByMeasure();
  testColorFollowsVisits();
  testDegenerateInputs();
  std::println("workbench color checks passed");
  return 0;
}
