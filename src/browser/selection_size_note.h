#pragma once

#include "browser/directory_model.h"

#include <string>

namespace luciole {
  // Why the size of a selection reads "at least": "Not counted: 2 of 5 selected files have no known size." Empty when
  // every selected file has a size.
  std::string selectionSizeNote(const SelectionSummary& summary);
} // namespace luciole
