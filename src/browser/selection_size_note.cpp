#include "browser/selection_size_note.h"

#include <format>

namespace luciole {
  std::string selectionSizeNote(const SelectionSummary& summary) {
    if (summary.unknownSizes == 0) {
      return {};
    }
    return std::format(
        "Not counted: {} of {} selected files {} no known size.", summary.unknownSizes, summary.files,
        summary.unknownSizes == 1 ? "has" : "have"
    );
  }
} // namespace luciole
