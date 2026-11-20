#include "formats/ascii.h"
#include "test_check.h"

#include <print>

namespace {
  using namespace luciole::formats;

  void testDigits() {
    TEST_CHECK(isAsciiDigit('0'));
    TEST_CHECK(isAsciiDigit('9'));
    TEST_CHECK(!isAsciiDigit('a'));
    TEST_CHECK(!isAsciiDigit('/'));
    TEST_CHECK(!isAsciiDigit(':'));
    TEST_CHECK(!isAsciiDigit('\xD9'));
  }

  void testLetters() {
    TEST_CHECK(isAsciiAlpha('a'));
    TEST_CHECK(isAsciiAlpha('z'));
    TEST_CHECK(isAsciiAlpha('A'));
    TEST_CHECK(isAsciiAlpha('Z'));
    TEST_CHECK(!isAsciiAlpha('@'));
    TEST_CHECK(!isAsciiAlpha('['));
    TEST_CHECK(!isAsciiAlpha('`'));
    TEST_CHECK(!isAsciiAlpha('{'));
    TEST_CHECK(!isAsciiAlpha('5'));
    TEST_CHECK(!isAsciiAlpha('\xC3'));
  }

  void testHexDigits() {
    TEST_CHECK(isAsciiHexDigit('0'));
    TEST_CHECK(isAsciiHexDigit('9'));
    TEST_CHECK(isAsciiHexDigit('a'));
    TEST_CHECK(isAsciiHexDigit('f'));
    TEST_CHECK(isAsciiHexDigit('A'));
    TEST_CHECK(isAsciiHexDigit('F'));
    TEST_CHECK(!isAsciiHexDigit('g'));
    TEST_CHECK(!isAsciiHexDigit('G'));
    TEST_CHECK(!isAsciiHexDigit(' '));
  }

  void testBlanks() {
    TEST_CHECK(isBlank(' '));
    TEST_CHECK(isBlank('\t'));
    TEST_CHECK(!isBlank('\n'));
    TEST_CHECK(!isBlank('\r'));
    TEST_CHECK(!isBlank('\v'));
    TEST_CHECK(!isBlank('x'));
  }

  void testLowercase() {
    TEST_CHECK(toAsciiLower('A') == 'a');
    TEST_CHECK(toAsciiLower('Z') == 'z');
    TEST_CHECK(toAsciiLower('a') == 'a');
    TEST_CHECK(toAsciiLower('@') == '@');
    TEST_CHECK(toAsciiLower('[') == '[');
    TEST_CHECK(toAsciiLower('\xC3') == '\xC3');
  }

  void testEqualsIgnoringCase() {
    TEST_CHECK(equalsIgnoringCase("URL(", "url("));
    TEST_CHECK(equalsIgnoringCase("Deg", "deg"));
    TEST_CHECK(equalsIgnoringCase("", ""));
    TEST_CHECK(!equalsIgnoringCase("deg", "de"));
    TEST_CHECK(!equalsIgnoringCase("rad", "deg"));
    // The second text is lowercase already: an uppercase letter in it never matches.
    TEST_CHECK(!equalsIgnoringCase("deg", "DEG"));
    TEST_CHECK(!equalsIgnoringCase("\xC3\x89", "\xC3\xA9"));
  }

  void testSkipBlanks() {
    TEST_CHECK(skipBlanks("  \tx", 0) == 3);
    TEST_CHECK(skipBlanks("x  ", 1) == 3);
    TEST_CHECK(skipBlanks("x\n", 1) == 1);
    TEST_CHECK(skipBlanks("ab", 5) == 5);
    TEST_CHECK(skipBlanks("", 0) == 0);
  }
} // namespace

int main() {
  testDigits();
  testLetters();
  testHexDigits();
  testBlanks();
  testLowercase();
  testEqualsIgnoringCase();
  testSkipBlanks();
  std::println("formats ascii checks passed");
  return 0;
}
