#pragma once

#include "fs/location.h"

#include <vector>

namespace luciole {
  struct Breadcrumb {
    Location location;
    std::string label;
  };

  // URI ancestry with a Home display root; never queries directories or reconstructs targets from labels.
  std::vector<Breadcrumb> locationBreadcrumbs(const Location& location);
  std::string locationEditText(const Location& location);
  // The location's own name, for a title: "Trash" and "Network" for those two, else its file name or its URI.
  std::string locationTitle(const Location& location);
  Location resolveLocationText(const std::string& text, const Location& base);
} // namespace luciole
