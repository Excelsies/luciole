#pragma once

#include <cstdlib>

// A time an absolute bound is set on, in a test: a slow or shared machine, a sanitizer or a software renderer goes
// over it without any fault, so the unit run only prints the time. The bounds are held where they are run on
// purpose: `meson test --benchmark`, which sets LUCIOLE_CPU_BUDGET. Checks that compare two timings from the same
// process stay in the unit run.
namespace test {
  inline bool budgetEnforced() { return std::getenv("LUCIOLE_CPU_BUDGET") != nullptr; }

  // True when `measured` is under `bound`, or when the bounds are not enforced.
  inline bool withinBudget(double measured, double bound) { return !budgetEnforced() || measured < bound; }
} // namespace test

namespace test::audio {
  inline constexpr double kMaxMeanCpuMilliseconds = 0.5;

  inline bool withinCpuBudget(double meanMilliseconds) {
    return test::withinBudget(meanMilliseconds, kMaxMeanCpuMilliseconds);
  }
} // namespace test::audio
