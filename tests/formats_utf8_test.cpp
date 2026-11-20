#include "formats/utf8.h"
#include "test_check.h"

#include <string>

namespace {
  using luciole::formats::appendUtf8;

  std::string encoded(std::uint32_t code) {
    std::string out = "<";
    appendUtf8(out, code);
    return out;
  }

  void testEncodedLengths() {
    TEST_CHECK(encoded(0x41) == "<A");
    TEST_CHECK(encoded(0x7F) == "<\x7F");
    TEST_CHECK(encoded(0x80) == "<\xC2\x80");
    TEST_CHECK(encoded(0xE9) == "<\xC3\xA9");
    TEST_CHECK(encoded(0x7FF) == "<\xDF\xBF");
    TEST_CHECK(encoded(0x800) == "<\xE0\xA0\x80");
    TEST_CHECK(encoded(0x20AC) == "<\xE2\x82\xAC");
    TEST_CHECK(encoded(0xFFFF) == "<\xEF\xBF\xBF");
    TEST_CHECK(encoded(0x10000) == "<\xF0\x90\x80\x80");
    TEST_CHECK(encoded(0x1F600) == "<\xF0\x9F\x98\x80");
    TEST_CHECK(encoded(0x10FFFF) == "<\xF4\x8F\xBF\xBF");
  }

  void testOutOfRangeIsReplaced() {
    TEST_CHECK(encoded(0x110000) == "<\xEF\xBF\xBD");
    TEST_CHECK(encoded(0xFFFFFFFF) == "<\xEF\xBF\xBD");
  }
} // namespace

int main() {
  testEncodedLengths();
  testOutOfRangeIsReplaced();
  return 0;
}
