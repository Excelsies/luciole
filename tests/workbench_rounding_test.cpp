#include "test_check.h"
#include "workbench/rounding.h"

#include <cmath>
#include <limits>
#include <print>

namespace {
  using namespace luciole::workbench;

  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInfinity = std::numeric_limits<double>::infinity();

  void testHalvesGoUp() {
    TEST_CHECK(roundHalfUp(2.5) == 3.0);
    TEST_CHECK(roundHalfUp(0.5) == 1.0);
    TEST_CHECK(roundHalfUp(-0.5) == 0.0);
    TEST_CHECK(roundHalfUp(-2.5) == -2.0);
    TEST_CHECK(roundHalfUp(-3.5) == -3.0);
  }

  void testOtherValuesGoToTheNearest() {
    TEST_CHECK(roundHalfUp(2.4) == 2.0);
    TEST_CHECK(roundHalfUp(2.6) == 3.0);
    TEST_CHECK(roundHalfUp(-2.4) == -2.0);
    TEST_CHECK(roundHalfUp(-2.6) == -3.0);
    TEST_CHECK(roundHalfUp(0.49999999999999994) == 0.0);
    TEST_CHECK(roundHalfUp(4503599627370497.0) == 4503599627370497.0);
    TEST_CHECK(roundHalfUp(7.0) == 7.0);
  }

  void testSmallNegativesGiveUnsignedZero() {
    TEST_CHECK(!std::signbit(roundHalfUp(-0.3)));
    TEST_CHECK(!std::signbit(roundHalfUp(-0.5)));
  }

  void testNonFiniteValuesPassThrough() {
    TEST_CHECK(std::isnan(roundHalfUp(kNan)));
    TEST_CHECK(roundHalfUp(kInfinity) == kInfinity);
    TEST_CHECK(roundHalfUp(-kInfinity) == -kInfinity);
  }
} // namespace

int main() {
  testHalvesGoUp();
  testOtherValuesGoToTheNearest();
  testSmallNegativesGiveUnsignedZero();
  testNonFiniteValuesPassThrough();
  std::println("workbench rounding checks passed");
  return 0;
}
