#include "test_check.h"
#include "workbench/atlas_text.h"
#include "workbench/flame_row.h"

#include <array>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;

  // Every ASCII character 6 px wide at 12 px, any other code point 12 px.
  LabelFont evenFont() {
    LabelFont font;
    font.ascii.fill(6);
    font.other = 12;
    return font;
  }

  void widthsScaleWithSize() {
    const auto font = evenFont();
    TEST_NEAR(atlasTextWidth("abc", font, 12), 18.0, 1e-12);
    TEST_NEAR(atlasTextWidth("abc", font, 13), 19.5, 1e-12);
    TEST_NEAR(atlasTextWidth("\xC3\xA9", font, 12), 12.0, 1e-12);
    TEST_NEAR(atlasTextWidth("abc", font, -1), 0.0, 1e-12);
  }

  // A text that does not fit is cut in the middle, keeping at least three code points on each side of the ellipsis.
  void fittingCutsInTheMiddle() {
    const auto font = evenFont();
    TEST_CHECK(fitAtlasText("abcdefghij", 60, font, 12) == "abcdefghij");
    TEST_CHECK(fitAtlasText("abcdefghij", 59, font, 12) == "abcd\xE2\x80\xA6hij");
    TEST_CHECK(fitAtlasText("abcdefghij", 48, font, 12) == "abc\xE2\x80\xA6hij");
    TEST_CHECK(fitAtlasText("abcdefghij", 47, font, 12).empty());
    TEST_CHECK(fitAtlasText("abcdefg", 41, font, 12).empty());
    TEST_CHECK(fitAtlasText("ab", 17, font, 12).empty());
    TEST_CHECK(fitAtlasText("ab", 18, font, 12) == "ab");
    TEST_CHECK(fitAtlasText("", 100, font, 12).empty());
  }

  // A cut name keeps its extension whole after the ellipsis, in a chip and in a circle alike.
  void fittingKeepsTheExtension() {
    const auto font = evenFont();
    const std::string name = "a-very-long-column.txt";
    TEST_CHECK(fitAtlasText(name, 66, font, 12) == "a-ver\xE2\x80\xA6.txt");
    TEST_CHECK(fitAtlasText(name, 54, font, 12) == "a-v\xE2\x80\xA6.txt");
    TEST_CHECK(fitAtlasText(name, 53, font, 12).empty());
    // A circle of 36 px leaves 59.8 px for one line at its middle.
    TEST_CHECK((atlasNameRows(name, 36, 17, 17, font, 12) == std::vector<std::string>{"a-v\xE2\x80\xA6.txt"}));
  }

  // Two lines split after the separator that balances them best, else in the middle.
  void namesSplitAtTheBestSeparator() {
    const auto font = evenFont();
    TEST_CHECK((splitAtlasName("alpha-beta", font, 12) == std::array<std::string, 2>{"alpha-", "beta"}));
    TEST_CHECK((splitAtlasName("abcdefghij", font, 12) == std::array<std::string, 2>{"abcde", "fghij"}));
    TEST_CHECK((splitAtlasName("ab.cdefg_hijk", font, 12) == std::array<std::string, 2>{"ab.cdefg_", "hijk"}));
    // Equal splits keep the first.
    TEST_CHECK((splitAtlasName("ab.cdefgh_ijk", font, 12) == std::array<std::string, 2>{"ab.", "cdefgh_ijk"}));
    TEST_CHECK((splitAtlasName("a_bcdef.ghijkl", font, 12) == std::array<std::string, 2>{"a_bcdef.", "ghijkl"}));
  }

  void rowsFitTheCircle() {
    const auto font = evenFont();
    // 5 code points at 13 px are 32.5 px: one line.
    TEST_CHECK((atlasNameRows("short", 100, 17, 17, font, 13) == std::vector<std::string>{"short"}));
    // 40 code points (260 px) in a circle of 100 px: two lines of 20 that fit whole.
    const std::string forty(40, 'a');
    const auto two = atlasNameRows(forty, 100, 17, 17, font, 13);
    TEST_CHECK(two.size() == 2 && two[0] == std::string(20, 'a') && two[1] == std::string(20, 'a'));
    // 60 code points: each half is cut; a circle of 56 px or more keeps both cut lines, a smaller one one line.
    const std::string sixty(60, 'b');
    const auto cutTwo = atlasNameRows(sixty, 100, 17, 17, font, 13);
    TEST_CHECK(cutTwo.size() == 2);
    TEST_CHECK(cutTwo[0].contains("\xE2\x80\xA6"));
    TEST_CHECK(atlasTextWidth(cutTwo[0], font, 13) <= 179.3);
    const auto cutOne = atlasNameRows(sixty, 50, 17, 17, font, 13);
    TEST_CHECK(cutOne.size() == 1 && cutOne[0].contains("\xE2\x80\xA6"));
    // A small circle never splits: one cut line, or nothing.
    const auto small = atlasNameRows(forty, 30, 17, 17, font, 12);
    TEST_CHECK(small.size() == 1 && atlasTextWidth(small[0], font, 12) <= 48.2);
    TEST_CHECK(atlasNameRows(forty, 10, 17, 17, font, 12).empty());
    TEST_CHECK(atlasNameRows("", 100, 17, 17, font, 12).empty());
  }
} // namespace

int main() {
  widthsScaleWithSize();
  fittingCutsInTheMiddle();
  fittingKeepsTheExtension();
  namesSplitAtTheBestSeparator();
  rowsFitTheCircle();
  return 0;
}
