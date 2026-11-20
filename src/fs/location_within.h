#pragma once

#include "fs/location.h"

namespace luciole {
  // `location` is `root` or lies below it.
  inline bool locationWithin(const Location& location, const Location& root) {
    const auto& prefix = root.uri();
    return location == root || location.uri().starts_with(prefix + (prefix.ends_with('/') ? "" : "/"));
  }
} // namespace luciole
