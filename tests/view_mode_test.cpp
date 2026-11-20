#include "model/directory_options.h"
#include "test_check.h"

#include <print>

namespace {
  void testDetailsRows() {
    TEST_CHECK(luciole::showsDetailsRows(luciole::ViewMode::List));
    TEST_CHECK(luciole::showsDetailsRows(luciole::ViewMode::Workbench));
    TEST_CHECK(!luciole::showsDetailsRows(luciole::ViewMode::Grid));
    TEST_CHECK(!luciole::showsDetailsRows(luciole::ViewMode::Miller));
  }

  void testModeAfterVisit() {
    using luciole::ViewMode;
    using luciole::viewModeAfterVisit;
    // Column view stays on whatever the visit saved.
    TEST_CHECK(viewModeAfterVisit(ViewMode::Miller, true, ViewMode::List) == ViewMode::Miller);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Miller, true, ViewMode::Grid) == ViewMode::Miller);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Workbench, true, ViewMode::Grid) == ViewMode::Miller);
    // Workbench stays on whatever the visit saved.
    TEST_CHECK(viewModeAfterVisit(ViewMode::Workbench, false, ViewMode::List) == ViewMode::Workbench);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Workbench, false, ViewMode::Grid) == ViewMode::Workbench);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Workbench, false, ViewMode::Miller) == ViewMode::Workbench);
    // List and Grid take the visit's saved mode.
    TEST_CHECK(viewModeAfterVisit(ViewMode::List, false, ViewMode::Grid) == ViewMode::Grid);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Grid, false, ViewMode::List) == ViewMode::List);
    TEST_CHECK(viewModeAfterVisit(ViewMode::List, false, ViewMode::Workbench) == ViewMode::Workbench);
    TEST_CHECK(viewModeAfterVisit(ViewMode::Grid, false, ViewMode::Grid) == ViewMode::Grid);
    // Column view switched off leaves Miller for the visit's saved mode.
    TEST_CHECK(viewModeAfterVisit(ViewMode::Miller, false, ViewMode::List) == ViewMode::List);
  }
} // namespace

int main() {
  testDetailsRows();
  testModeAfterVisit();
  std::println("view mode checks passed");
  return 0;
}
