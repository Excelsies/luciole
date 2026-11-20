#include "browser/directory_model.h"
#include "browser/selection_size_note.h"
#include "test_check.h"

// The status bar writes "at least" before the size of a selection when some selected files have no known size; its
// tooltip says how many.
namespace {
  using luciole::selectionSizeNote;
  using luciole::SelectionSummary;

  void saysHowManyFilesHaveNoSize() {
    SelectionSummary summary;
    summary.files = 5;
    summary.unknownSizes = 2;
    TEST_CHECK(selectionSizeNote(summary) == "Not counted: 2 of 5 selected files have no known size.");
    summary.unknownSizes = 1;
    TEST_CHECK(selectionSizeNote(summary) == "Not counted: 1 of 5 selected files has no known size.");
  }

  void saysNothingWhenEverySizeIsKnown() {
    SelectionSummary summary;
    summary.files = 5;
    TEST_CHECK(selectionSizeNote(summary).empty());
    TEST_CHECK(selectionSizeNote({}).empty());
  }
} // namespace

int main() {
  saysHowManyFilesHaveNoSize();
  saysNothingWhenEverySizeIsKnown();
  return 0;
}
