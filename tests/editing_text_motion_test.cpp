#include "editing/text_motion.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::editing;

  TextBuffer make(std::string_view text) {
    auto buffer = TextBuffer::create(text);
    TEST_CHECK(buffer.has_value());
    return std::move(*buffer);
  }

  // e + combining acute, a family of three joined by ZWJ, flags of France and Germany, a thumbs up with a skin
  // tone modifier, a heart with the emoji variation selector.
  constexpr std::string_view kAccent = "e\xCC\x81";
  constexpr std::string_view kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  constexpr std::string_view kFlags = "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA";
  constexpr std::string_view kThumb = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD";
  constexpr std::string_view kHeart = "\xE2\x9D\xA4\xEF\xB8\x8F";

  void testClusterEnds() {
    TEST_CHECK(clusterEnd(kAccent, 0) == 3);
    TEST_CHECK(clusterEnd(kFamily, 0) == kFamily.size());
    TEST_CHECK(clusterEnd(kFlags, 0) == 8 && clusterEnd(kFlags, 8) == 16);
    TEST_CHECK(clusterEnd(kThumb, 0) == 8 && clusterEnd(kHeart, 0) == 6);
    TEST_CHECK(clusterEnd("ab", 2) == 2 && clusterEnd("ab", 0) == 1);
    // A lone regional indicator after a pair is its own cluster.
    const std::string threeIndicators = std::string(kFlags.substr(0, 12));
    TEST_CHECK(clusterEnd(threeIndicators, 8) == 12);
    TEST_CHECK(clusterStart(threeIndicators, 12) == 8 && clusterStart(threeIndicators, 8) == 0);
    // A regional indicator does not take a following letter.
    TEST_CHECK(clusterEnd(std::string(kFlags.substr(0, 4)) + "a", 0) == 4);
    // A trailing ZWJ joins nothing after it.
    TEST_CHECK(clusterEnd("a\xE2\x80\x8D", 0) == 4);
    TEST_CHECK(clusterStart(kAccent, 3) == 0 && clusterStart(kAccent, 0) == 0);
  }

  void testClusterMotions() {
    const auto text = std::string(kAccent) + "x" + std::string(kFamily) + std::string(kFlags) + "\nz";
    const auto buffer = make(text);
    TEST_CHECK((clusterRight(buffer, {0, 0}) == TextPosition{0, 3}));
    TEST_CHECK((clusterRight(buffer, {0, 1}) == TextPosition{0, 3}));
    TEST_CHECK((clusterRight(buffer, {0, 3}) == TextPosition{0, 4}));
    TEST_CHECK((clusterRight(buffer, {0, 4}) == TextPosition{0, 22}));
    TEST_CHECK((clusterRight(buffer, {0, 22}) == TextPosition{0, 30}));
    TEST_CHECK((clusterRight(buffer, {0, 30}) == TextPosition{0, 38}));
    TEST_CHECK((clusterRight(buffer, {0, 38}) == TextPosition{1, 0}));
    TEST_CHECK((clusterRight(buffer, {1, 1}) == TextPosition{1, 1}));
    TEST_CHECK((clusterLeft(buffer, {1, 0}) == TextPosition{0, 38}));
    TEST_CHECK((clusterLeft(buffer, {0, 38}) == TextPosition{0, 30}));
    TEST_CHECK((clusterLeft(buffer, {0, 22}) == TextPosition{0, 4}));
    TEST_CHECK((clusterLeft(buffer, {0, 3}) == TextPosition{0, 0}));
    TEST_CHECK((clusterLeft(buffer, {0, 2}) == TextPosition{0, 0}));
    TEST_CHECK((clusterLeft(buffer, {0, 0}) == TextPosition{0, 0}));
    // Clamped first: inside the family's first code point is its start.
    TEST_CHECK((clusterRight(buffer, {0, 6}) == TextPosition{0, 22}));
  }

  // x = cafe-acute_1+<<q>>  na-diaeresis-ve, with Latin-1 guillemets as punctuation.
  constexpr std::string_view kWords = "x = caf\xC3\xA9_1+\xC2\xABq\xC2\xBB  na\xC3\xAFve\na\xC2\xA0"
                                      "b\xE2\x80\x93"
                                      "c\n";

  void testWordRight() {
    const auto buffer = make(kWords);
    TEST_CHECK((wordRight(buffer, {0, 0}) == TextPosition{0, 1}));
    TEST_CHECK((wordRight(buffer, {0, 1}) == TextPosition{0, 3}));
    TEST_CHECK((wordRight(buffer, {0, 3}) == TextPosition{0, 11}));
    TEST_CHECK((wordRight(buffer, {0, 11}) == TextPosition{0, 14}));
    TEST_CHECK((wordRight(buffer, {0, 14}) == TextPosition{0, 15}));
    TEST_CHECK((wordRight(buffer, {0, 15}) == TextPosition{0, 17}));
    TEST_CHECK((wordRight(buffer, {0, 17}) == TextPosition{0, 25}));
    TEST_CHECK((wordRight(buffer, {0, 25}) == TextPosition{1, 0}));
    // A no-break space is white space; an en dash is punctuation.
    TEST_CHECK((wordRight(buffer, {1, 0}) == TextPosition{1, 1}));
    TEST_CHECK((wordRight(buffer, {1, 1}) == TextPosition{1, 4}));
    TEST_CHECK((wordRight(buffer, {1, 4}) == TextPosition{1, 7}));
    TEST_CHECK((wordRight(buffer, {1, 7}) == TextPosition{1, 8}));
    TEST_CHECK((wordRight(buffer, {2, 0}) == TextPosition{2, 0}));
  }

  void testWordLeft() {
    const auto buffer = make(kWords);
    TEST_CHECK((wordLeft(buffer, {0, 25}) == TextPosition{0, 19}));
    TEST_CHECK((wordLeft(buffer, {0, 19}) == TextPosition{0, 15}));
    TEST_CHECK((wordLeft(buffer, {0, 15}) == TextPosition{0, 14}));
    TEST_CHECK((wordLeft(buffer, {0, 14}) == TextPosition{0, 11}));
    TEST_CHECK((wordLeft(buffer, {0, 11}) == TextPosition{0, 4}));
    TEST_CHECK((wordLeft(buffer, {0, 4}) == TextPosition{0, 2}));
    TEST_CHECK((wordLeft(buffer, {0, 2}) == TextPosition{0, 0}));
    TEST_CHECK((wordLeft(buffer, {0, 0}) == TextPosition{0, 0}));
    TEST_CHECK((wordLeft(buffer, {1, 0}) == TextPosition{0, 25}));
    TEST_CHECK((wordLeft(buffer, {1, 4}) == TextPosition{1, 3}));
    TEST_CHECK((wordLeft(buffer, {1, 3}) == TextPosition{1, 0}));
  }

  void testWordRange() {
    const auto buffer = make(kWords);
    TEST_CHECK((wordRange(buffer, {0, 5}) == TextSelection{{0, 4}, {0, 11}}));
    TEST_CHECK((wordRange(buffer, {0, 4}) == TextSelection{{0, 4}, {0, 11}}));
    TEST_CHECK((wordRange(buffer, {0, 12}) == TextSelection{{0, 11}, {0, 14}}));
    TEST_CHECK((wordRange(buffer, {0, 18}) == TextSelection{{0, 17}, {0, 19}}));
    TEST_CHECK((wordRange(buffer, {0, 25}) == TextSelection{{0, 19}, {0, 25}}));
    TEST_CHECK((wordRange(buffer, {2, 0}) == TextSelection{{2, 0}, {2, 0}}));
    // Clusters stay whole: an accent after a letter belongs to the word.
    const auto accented = make("(" + std::string(kAccent) + "t)");
    TEST_CHECK((wordRange(accented, {0, 2}) == TextSelection{{0, 1}, {0, 5}}));
  }

  void testLineMotions() {
    const auto buffer = make("    foo\n\tbar\n   \nlast");
    TEST_CHECK((lineStart(buffer, {0, 6}) == TextPosition{0, 4}));
    TEST_CHECK((lineStart(buffer, {0, 4}) == TextPosition{0, 0}));
    TEST_CHECK((lineStart(buffer, {0, 0}) == TextPosition{0, 4}));
    TEST_CHECK((lineStart(buffer, {0, 2}) == TextPosition{0, 4}));
    TEST_CHECK((lineStart(buffer, {1, 4}) == TextPosition{1, 1}));
    TEST_CHECK((lineStart(buffer, {2, 1}) == TextPosition{2, 3}));
    TEST_CHECK((lineStart(buffer, {2, 3}) == TextPosition{2, 0}));
    TEST_CHECK((lineEnd(buffer, {0, 1}) == TextPosition{0, 7}));
    TEST_CHECK((lineEnd(buffer, {3, 0}) == TextPosition{3, 4}));
    TEST_CHECK((documentStart(buffer) == TextPosition{0, 0}));
    TEST_CHECK((documentEnd(buffer) == TextPosition{3, 4}));
    TEST_CHECK((lineRange(buffer, 1) == TextSelection{{1, 0}, {2, 0}}));
    TEST_CHECK((lineRange(buffer, 3) == TextSelection{{3, 0}, {3, 4}}));
    TEST_CHECK((lineRange(buffer, 9) == TextSelection{{3, 0}, {3, 4}}));
    TEST_CHECK((selectAll(buffer) == TextSelection{{0, 0}, {3, 4}}));
    const auto trailing = make("a\n");
    TEST_CHECK((documentEnd(trailing) == TextPosition{1, 0}));
    TEST_CHECK((selectAll(trailing) == TextSelection{{0, 0}, {1, 0}}));
  }
} // namespace

int main() {
  testClusterEnds();
  testClusterMotions();
  testWordRight();
  testWordLeft();
  testWordRange();
  testLineMotions();
  std::println("editing_text_motion_test: ok");
  return 0;
}
