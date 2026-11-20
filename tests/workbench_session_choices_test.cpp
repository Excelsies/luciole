#include "app/page/workbench_session.h"
#include "core/input/key_modifiers.h"
#include "test_check.h"

#include <linux/input-event-codes.h>
#include <print>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// What the page does with a row chosen from a merged piece's list, and the keys it takes in Workbench.
namespace {
  using luciole::Location;
  using luciole::MoreAction;

  void chosenRowsDoWhatTheirPieceDoes() {
    const auto folder = Location::fromPath("/data/photos");
    const std::vector<luciole::MergedEntry> listed{
        {.entry = {.location = folder.child("2024"), .folder = true}, .name = "2024", .value = "3.1 GiB"},
        {.entry = {.location = folder.child("cover.jpg"), .folder = false}, .name = "cover.jpg", .value = "812 KiB"},
    };
    const auto entered = luciole::moreChoice(listed, 1);
    TEST_CHECK(entered && entered->action == MoreAction::Go);
    TEST_CHECK(entered->request.location == folder.child("2024") && entered->request.button == BTN_LEFT);
    const auto selected = luciole::moreChoice(listed, 2);
    TEST_CHECK(selected && selected->action == MoreAction::Select);
    TEST_CHECK(selected->request.location == folder.child("cover.jpg") && selected->request.modifiers == 0);
    // the heading, the unlisted row and ids no row has choose nothing
    for (const int id : {0, 3, -1, 1 << 30}) {
      TEST_CHECK(!luciole::moreChoice(listed, id));
    }
    TEST_CHECK(!luciole::moreChoice({}, 1));
  }

  void workbenchTakesTheDockedPreviewKey() {
    using luciole::ViewMode;
    TEST_CHECK(luciole::takesPreviewKey(ViewMode::Workbench, XKB_KEY_F8, 0));
    for (const auto mode : {ViewMode::List, ViewMode::Grid, ViewMode::Miller}) {
      TEST_CHECK(!luciole::takesPreviewKey(mode, XKB_KEY_F8, 0));
    }
    TEST_CHECK(!luciole::takesPreviewKey(ViewMode::Workbench, XKB_KEY_F7, 0));
    TEST_CHECK(!luciole::takesPreviewKey(ViewMode::Workbench, XKB_KEY_F8, KeyMod::Ctrl));
  }
} // namespace

int main() {
  chosenRowsDoWhatTheirPieceDoes();
  workbenchTakesTheDockedPreviewKey();
  std::println("workbench session choices checks passed");
}
