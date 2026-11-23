#pragma once

#include <cstdlib>

// Processor time one advance and draw of an audio visual may take, in the debug build. A slow or shared machine, or
// a software renderer, goes over it without any fault, so the unit run only prints the time. The budget is held where
// it is run on purpose: `meson test --benchmark`, which sets LUCIOLE_CPU_BUDGET.
namespace test::audio {
  inline constexpr double kMaxMeanCpuMilliseconds = 0.5;

  inline bool withinCpuBudget(double meanMilliseconds) {
    return std::getenv("LUCIOLE_CPU_BUDGET") == nullptr || meanMilliseconds < kMaxMeanCpuMilliseconds;
  }
} // namespace test::audio
