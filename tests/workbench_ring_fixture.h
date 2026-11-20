#pragma once

#include "workbench/ring_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

namespace luciole::test::ring {
  using namespace workbench::ring;

  inline constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  inline constexpr double kInfinity = std::numeric_limits<double>::infinity();
  inline constexpr double kClose = 1e-6;

  inline double sum(const std::vector<double>& values) { return std::accumulate(values.begin(), values.end(), 0.0); }

  // Deterministic pseudo random weights, heaviest first, so failures reproduce.
  inline std::vector<double> skewed(std::size_t count, std::uint32_t seed, double power) {
    std::uint32_t state = seed;
    std::vector<double> weights;
    weights.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      state = state * 1664525U + 1013904223U;
      weights.push_back(std::pow(static_cast<double>(state) / 4294967296.0, power) + 1e-4);
    }
    std::ranges::sort(weights, std::greater{});
    return weights;
  }

  inline std::vector<double> mids(const std::vector<double>& angles) {
    const auto starts = arcStarts(angles);
    std::vector<double> result;
    result.reserve(angles.size());
    for (std::size_t i = 0; i < angles.size(); ++i) {
      result.push_back(starts[i] + angles[i] / 2);
    }
    return result;
  }
} // namespace luciole::test::ring
