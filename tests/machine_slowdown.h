#pragma once

#include "test_check.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// How much slower than a quiet machine this thread's processor time runs now. Bounds in processor time still grow when
// other busy processes share caches and cores; a test divides its measured cost by this before comparing.
namespace luciole::test {

  inline double referenceThreadMs() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1e3) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  // A fixed amount of the kind of work the code under test does (allocating, comparing and moving strings), in thread
  // processor time.
  inline double referenceWorkMs() {
    constexpr std::size_t kNames = 20000;
    const double start = referenceThreadMs();
    std::vector<std::string> names;
    names.reserve(kNames);
    std::uint32_t seed = 12345;
    for (std::size_t i = 0; i < kNames; ++i) {
      seed = (seed * 1664525U) + 1013904223U;
      names.push_back("n" + std::to_string(seed >> 8));
    }
    std::ranges::sort(names);
    const double spent = referenceThreadMs() - start;
    TEST_CHECK(names.size() == kNames && names.front() <= names.back());
    return spent;
  }

  // The quietest of a few reference samples against their time on a quiet machine, never below 1.
  inline double machineSlowdown() {
    constexpr int kSamples = 5;
    constexpr double kQuietReferenceMs = 20.0;
    double best = referenceWorkMs();
    for (int i = 1; i < kSamples; ++i) {
      best = std::min(best, referenceWorkMs());
    }
    return std::max(1.0, best / kQuietReferenceMs);
  }

} // namespace luciole::test
