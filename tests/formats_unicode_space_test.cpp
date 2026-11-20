#include "formats/unicode_space.h"
#include "test_check.h"

#include <print>

namespace {
  using namespace luciole::formats;

  void testWidths() {
    TEST_CHECK(unicodeSpaceWidth(" x", 0) == 1);
    TEST_CHECK(unicodeSpaceWidth("\t", 0) == 1);
    TEST_CHECK(unicodeSpaceWidth("\v", 0) == 1);
    TEST_CHECK(unicodeSpaceWidth("\r", 0) == 1);
    TEST_CHECK(unicodeSpaceWidth("\xC2\xA0", 0) == 2);
    TEST_CHECK(unicodeSpaceWidth("\xE1\x9A\x80", 0) == 3);
    TEST_CHECK(unicodeSpaceWidth("\xE2\x80\x8A", 0) == 3);
    TEST_CHECK(unicodeSpaceWidth("\xE2\x80\xA9", 0) == 3);
    TEST_CHECK(unicodeSpaceWidth("\xEF\xBB\xBF", 0) == 3);
    // U+200B, the zero width space, is not white space.
    TEST_CHECK(unicodeSpaceWidth("\xE2\x80\x8B", 0) == 0);
    TEST_CHECK(unicodeSpaceWidth("x", 0) == 0);
    TEST_CHECK(unicodeSpaceWidth("ab", 5) == 0);
    TEST_CHECK(unicodeSpaceWidth("\xE2\x80", 0) == 0);
    TEST_CHECK(unicodeSpaceWidth("a\xC2\xA0", 1) == 2);
  }

  void testRuns() {
    TEST_CHECK(leadingUnicodeSpace(" \t\xC2\xA0x ") == 4);
    TEST_CHECK(leadingUnicodeSpace(" \xE3\x80\x80") == 4);
    TEST_CHECK(leadingUnicodeSpace("") == 0);
    TEST_CHECK(leadingUnicodeSpace("x ") == 0);
    TEST_CHECK(trailingUnicodeSpace("x \xE2\x80\xA8") == 4);
    TEST_CHECK(trailingUnicodeSpace("\xC2\xA0") == 2);
    TEST_CHECK(trailingUnicodeSpace("x\xC2") == 0);
    TEST_CHECK(trailingUnicodeSpace("") == 0);
  }
} // namespace

int main() {
  testWidths();
  testRuns();
  std::println("formats unicode space checks passed");
  return 0;
}
