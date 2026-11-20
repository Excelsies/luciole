#include "test_check.h"
#include "workbench/format.h"
#include "workbench_fixture.h"

#include <limits>
#include <print>
#include <string>

namespace {
  using namespace luciole::workbench;
  using luciole::test::referenceTree;

  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kKilo = 1024.0;

  void testSizeText() {
    TEST_CHECK(sizeText(0) == "0 bytes");
    TEST_CHECK(sizeText(1) == "1 byte");
    TEST_CHECK(sizeText(1023) == "1023 bytes");
    TEST_CHECK(sizeText(1024) == "1.0\u00A0KiB");
    TEST_CHECK(sizeText(4000) == "3.9\u00A0KiB");
    TEST_CHECK(sizeText(4.2 * kKilo * kKilo * kKilo) == "4.2\u00A0GiB");
    TEST_CHECK(sizeText(37 * kKilo * kKilo) == "37.0\u00A0MiB");
    TEST_CHECK(sizeText(kKilo * kKilo - 1) == "1024.0\u00A0KiB");
    TEST_CHECK(sizeText(0.4) == "0 bytes");
    TEST_CHECK(sizeText(0.5) == "1 byte");
    TEST_CHECK(sizeText(1023.5) == "1.0\u00A0KiB");
    TEST_CHECK(sizeText(10188) == "9.9\u00A0KiB");
    TEST_CHECK(sizeText(10189) == "10.0\u00A0KiB");
    TEST_CHECK(sizeText(1280) == "1.2\u00A0KiB");
    TEST_CHECK(sizeText(1177.6) == "1.2\u00A0KiB");
    TEST_CHECK(sizeText(999.5 * kKilo) == "999.5\u00A0KiB");
    TEST_CHECK(sizeText(kKilo * kKilo * kKilo * kKilo * kKilo) == "1.0\u00A0PiB");
    TEST_CHECK(sizeText(kKilo * kKilo * kKilo * kKilo * kKilo * kKilo) == "1.0\u00A0EiB");
    TEST_CHECK(sizeText(1e30) == "16.0\u00A0EiB");
    TEST_CHECK(sizeText(-1).empty());
    TEST_CHECK(sizeText(kNan).empty());
    TEST_CHECK(sizeText(kInfinity).empty());
  }

  void testCountText() {
    TEST_CHECK(countText(1234567) == "1,234,567");
    TEST_CHECK(countText(12) == "12");
    TEST_CHECK(countText(0) == "0");
    TEST_CHECK(countText(999) == "999");
    TEST_CHECK(countText(1000) == "1,000");
    TEST_CHECK(countText(-1234567) == "-1,234,567");
    TEST_CHECK(countText(0.5) == "1");
    TEST_CHECK(countText(2.5) == "3");
    TEST_CHECK(countText(-0.5) == "0");
    TEST_CHECK(countText(999999.5) == "1,000,000");
    TEST_CHECK(countText(1e20) == "100,000,000,000,000,000,000");
    TEST_CHECK(countText(kNan).empty());
    TEST_CHECK(countText(-kInfinity).empty());
    TEST_CHECK(itemsText(1) == "1 item");
    TEST_CHECK(itemsText(2000) == "2,000 items");
    TEST_CHECK(itemsText(0) == "0 items");
  }

  void testCountedText() {
    TEST_CHECK(countedText(1, "result") == "1 result");
    TEST_CHECK(countedText(0, "result") == "0 results");
    TEST_CHECK(countedText(12345, "folder") == "12,345 folders");
    TEST_CHECK(countedText(1, "item") == itemsText(1));
    TEST_CHECK(
        countedText(0.5, "item") == "1 item" && countedText(1.4, "item") == "1 item"
    ); // singular follows the printed count
    TEST_CHECK(countedText(1.5, "item") == "2 items");
    // A count that is not a number has no text, not a unit with a space before it.
    TEST_CHECK(countedText(kInfinity, "item").empty() && countedText(-kInfinity, "item").empty());
    TEST_CHECK(countedText(std::numeric_limits<double>::quiet_NaN(), "item").empty());
  }

  void testAgoText() {
    TEST_CHECK(agoText(10) == "just now");
    TEST_CHECK(agoText(5 * 60) == "5 min ago");
    TEST_CHECK(agoText(3 * 3600 + 5) == "3 h ago");
    TEST_CHECK(agoText(2 * 86400) == "2 d ago");
    TEST_CHECK(agoText(90 * 86400) == "3 mo ago");
    TEST_CHECK(agoText(800 * 86400) == "2 y ago");
    TEST_CHECK(agoText(-5) == "in the future");
    TEST_CHECK(agoText(0) == "just now");
    TEST_CHECK(agoText(59.9) == "just now");
    TEST_CHECK(agoText(60) == "1 min ago");
    TEST_CHECK(agoText(3599) == "59 min ago");
    TEST_CHECK(agoText(3600) == "1 h ago");
    TEST_CHECK(agoText(86399) == "23 h ago");
    TEST_CHECK(agoText(86400) == "1 d ago");
    TEST_CHECK(agoText(86400 * 30 - 1) == "29 d ago");
    TEST_CHECK(agoText(86400 * 30) == "1 mo ago");
    TEST_CHECK(agoText(86400 * 365 - 1) == "12 mo ago");
    TEST_CHECK(agoText(86400 * 365) == "1 y ago");
    TEST_CHECK(agoText(1e12) == "31709 y ago");
    TEST_CHECK(agoText(kNan).empty());
    TEST_CHECK(agoText(kInfinity).empty());
  }

  void testMiddleEllipsis() {
    TEST_CHECK(middleEllipsis("short.txt", 20) == "short.txt");
    TEST_CHECK(middleEllipsis("abcdefghij", 6) == "abc…ij");
    TEST_CHECK(middleEllipsis("abcdefghij", 1) == "…");
    TEST_CHECK(codePointCount(middleEllipsis(std::string(120, 'a'), 30)) == 30);
    TEST_CHECK(middleEllipsis("abc", 0).empty());
    TEST_CHECK(middleEllipsis("", 4).empty());
    TEST_CHECK(middleEllipsis("héllo wörld", 5) == "hé…ld");
    TEST_CHECK(middleEllipsis("a\U0001F600b\U0001F600c\U0001F600d", 4) == "a\U0001F600…d");
    TEST_CHECK(middleEllipsis("abc\xA9\xA9\xA9z", 3) == "a…z");
  }

  void testCodePointCount() {
    TEST_CHECK(codePointCount("") == 0);
    TEST_CHECK(codePointCount("abc") == 3);
    TEST_CHECK(codePointCount("héllo") == 5);
    TEST_CHECK(codePointCount("a\U0001F600b") == 3);
    // A stray continuation byte joins the code point before it, and the first byte always starts one.
    TEST_CHECK(codePointCount("abc\xA9\xA9\xA9z") == 4);
    TEST_CHECK(codePointCount("\x80") == 1);
    TEST_CHECK(codePointCount("\x80\x80x") == 2);
  }

  void testRelativePath() {
    const auto tree = referenceTree();
    TEST_CHECK(relativePath(tree, 9, 0) == "a/x/deep/g.c");
    TEST_CHECK(relativePath(tree, 9, 5) == "deep/g.c");
    TEST_CHECK(relativePath(tree, 5, 5) == "");
    TEST_CHECK(!relativePath(tree, 2, 5).has_value());
    TEST_CHECK(!relativePath(tree, 99, 0).has_value());
    TEST_CHECK(!relativePath(tree, 2, 99).has_value());
    auto cyclic = referenceTree();
    cyclic.nodes[0].parent = 9;
    TEST_CHECK(!relativePath(cyclic, 9, 2).has_value());
  }

  void testSplitRelativePath() {
    TEST_CHECK((splitRelativePath("lib/python3/x", 20) == PathSplit{"lib/python3/", "x"}));
    TEST_CHECK((splitRelativePath("lib/python3/site-packages/numpy", 12) == PathSplit{"lib…es/", "numpy"}));
    TEST_CHECK((splitRelativePath("lib/python3/site-packages/numpy", 8) == PathSplit{"l…/", "numpy"}));
    TEST_CHECK((splitRelativePath("averyveryverylongleafname", 10) == PathSplit{"", "avery…name"}));
    TEST_CHECK((splitRelativePath("a/b", 3) == PathSplit{"a/", "b"}));
    TEST_CHECK((splitRelativePath("abc/defghij", 9) == PathSplit{"", "defghij"}));
    TEST_CHECK((splitRelativePath("abc/defghij", 10) == PathSplit{"a…/", "defghij"}));
    TEST_CHECK((splitRelativePath("", 5) == PathSplit{"", ""}));
    TEST_CHECK((splitRelativePath("dir/", 4) == PathSplit{"dir/", ""}));
    TEST_CHECK((splitRelativePath("dir/leaf", 0) == PathSplit{"", ""}));
    TEST_CHECK((splitRelativePath("déjà/été", 8) == PathSplit{"déjà/", "été"}));
  }
} // namespace

int main() {
  testSizeText();
  testCountText();
  testCountedText();
  testAgoText();
  testMiddleEllipsis();
  testCodePointCount();
  testRelativePath();
  testSplitRelativePath();
  std::println("workbench format checks passed");
  return 0;
}
