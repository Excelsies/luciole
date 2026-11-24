#pragma once

#include <cstdlib>
#include <format>
#include <print>
#include <string_view>

// A time an absolute bound is set on, in a test: a slow or shared machine, a sanitizer or a software renderer goes
// over it without any fault, so the unit run prints the time and the bound and enforces nothing. The bounds are held
// where they are run on purpose: `meson test --benchmark`, which sets LUCIOLE_CPU_BUDGET. Checks that compare two
// timings from the same process stay in the unit run.
namespace test {
  inline bool budgetEnforced() { return std::getenv("LUCIOLE_CPU_BUDGET") != nullptr; }

  // True when `measured` is under `bound`, or when the bounds are not enforced.
  inline bool budgetHolds(double measured, double bound) { return !budgetEnforced() || measured < bound; }

  // The same, after printing what was measured, the bound and whether it counts here.
  inline bool withinBudget(double measured, double bound, std::string_view what) {
    std::println(
        "{}: {:.3f} against a bound of {} ({})", what, measured, bound, budgetEnforced() ? "enforced" : "not enforced"
    );
    return budgetHolds(measured, bound);
  }

  // A bound that guards against work growing faster than its input, such as a scan per element. A blow-up exceeds the
  // bound by hundreds of times, so the unit run keeps it at kGuardFactor times the bound, which a slow or shared
  // machine does not reach; the benchmark holds the bound itself.
  inline constexpr double kGuardFactor = 10;

  inline bool withinGuard(double measured, double bound, std::string_view what) {
    std::println(
        "{}: {:.3f} against a bound of {} ({})", what, measured, bound,
        budgetEnforced() ? "enforced" : std::format("guarded at {} times that", kGuardFactor)
    );
    return measured < (budgetEnforced() ? bound : bound * kGuardFactor);
  }
} // namespace test

// The measured value is its own label.
#define TEST_WITHIN_BUDGET(measured, bound) ::test::withinBudget((measured), (bound), #measured)
#define TEST_WITHIN_GUARD(measured, bound) ::test::withinGuard((measured), (bound), #measured)

namespace test::audio {
  inline constexpr double kMaxMeanCpuMilliseconds = 0.5;

  // The audio visuals print their own times.
  inline bool withinCpuBudget(double meanMilliseconds) {
    return test::budgetHolds(meanMilliseconds, kMaxMeanCpuMilliseconds);
  }
} // namespace test::audio
