#pragma once

#include <cmath>

namespace luciole::workbench {
  // Nearest integer, halves toward positive infinity; a value just under zero gives +0. NaN and infinities pass
  // through.
  inline double roundHalfUp(double value) {
    const double whole = std::floor(value);
    return value - whole >= 0.5 ? whole + 1.0 : whole;
  }
} // namespace luciole::workbench
