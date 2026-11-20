#include "formats/color_notations.h"
#include "test_check.h"

#include <optional>
#include <print>
#include <string>

namespace {
  using namespace luciole::formats;

  std::optional<ColorNotation> notation(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, std::size_t n) {
    return ColorNotation{.color = {.r = r, .g = g, .b = b, .a = a}, .length = n};
  }

  void testFunctions() {
    TEST_CHECK(cssColorFunction("rgb(255, 0, 128)") == notation(255, 0, 128, 255, 16));
    TEST_CHECK(cssColorFunction("RGBA(10 20 30 / 50%) tail") == notation(10, 20, 30, 128, 20));
    TEST_CHECK(cssColorFunction("hsl(120deg 100% 50%)") == notation(0, 255, 0, 255, 20));
    TEST_CHECK(cssColorFunction("hsla(0.5turn, 100%, 50%, 0.25)") == notation(0, 255, 255, 64, 30));
    TEST_CHECK(cssColorFunction("hwb(0 0% 0%)") == notation(255, 0, 0, 255, 12));
    TEST_CHECK(cssColorFunction("hwb(0 60% 60%)") == notation(128, 128, 128, 255, 14));
    // Channels clamp and hues wrap.
    TEST_CHECK(cssColorFunction("rgb(300 -5 0)") == notation(255, 0, 0, 255, 13));
    TEST_CHECK(cssColorFunction("hsl(-240 100% 50%)") == notation(0, 255, 0, 255, 18));
  }

  void testFunctionsRefused() {
    TEST_CHECK(!cssColorFunction(""));
    TEST_CHECK(!cssColorFunction("rgb"));
    TEST_CHECK(!cssColorFunction("rgb (1, 2, 3)"));
    TEST_CHECK(!cssColorFunction("rgbx(1, 2, 3)"));
    // A name of five or more letters is not a color function, whether or not it begins with one.
    TEST_CHECK(!cssColorFunction("rgbaa(1, 2, 3)"));
    TEST_CHECK(!cssColorFunction("hslaaaaaaaa(0 100% 50%)"));
    TEST_CHECK(!cssColorFunction("rgbaa"));
    // A byte past ASCII right after a valid name ends it without a match.
    TEST_CHECK(!cssColorFunction("rgb\xC3(1, 2, 3)"));
    TEST_CHECK(!cssColorFunction("rgba\xC3\xA9(1, 2, 3)"));
    TEST_CHECK(!cssColorFunction(" rgb(1, 2, 3)"));
    TEST_CHECK(!cssColorFunction("rgb(1, 2)"));
    TEST_CHECK(!cssColorFunction("rgb(1, 2 3)"));
    TEST_CHECK(!cssColorFunction("rgb(1, 2, 3"));
    TEST_CHECK(!cssColorFunction("rgb(1deg, 2, 3)"));
    TEST_CHECK(!cssColorFunction("hwb(0, 0%, 0%)"));
    TEST_CHECK(!cssColorFunction("hsl(10% 50% 50%)"));
    TEST_CHECK(!cssColorFunction("rgb(1e999, 2, 3)"));
    // The closing parenthesis must lie within kMaxColorFunctionBytes of the start.
    const std::string padded = "rgb(1, 2, 3" + std::string(kMaxColorFunctionBytes, ' ') + ")";
    TEST_CHECK(!cssColorFunction(padded));
    const std::string fits = "rgb(1, 2, 3" + std::string(kMaxColorFunctionBytes - 12, ' ') + ")";
    TEST_CHECK(cssColorFunction(fits) == notation(1, 2, 3, 255, kMaxColorFunctionBytes));
  }

  void testNames() {
    TEST_CHECK((cssColorName("rebeccapurple") == Rgba{.r = 0x66, .g = 0x33, .b = 0x99, .a = 255}));
    TEST_CHECK((cssColorName("AliceBlue") == Rgba{.r = 0xF0, .g = 0xF8, .b = 0xFF, .a = 255}));
    TEST_CHECK((cssColorName("transparent") == Rgba{.r = 0, .g = 0, .b = 0, .a = 0}));
    TEST_CHECK((cssColorName("lightgoldenrodyellow") == Rgba{.r = 0xFA, .g = 0xFA, .b = 0xD2, .a = 255}));
    TEST_CHECK(!cssColorName(""));
    TEST_CHECK(!cssColorName("blu"));
    TEST_CHECK(!cssColorName("bluex"));
    TEST_CHECK(!cssColorName("lightgoldenrodyellowx"));
    TEST_CHECK(!cssColorName(std::string(4096, 'a')));
  }
} // namespace

int main() {
  testFunctions();
  testFunctionsRefused();
  testNames();
  std::println("formats color notations checks passed");
  return 0;
}
