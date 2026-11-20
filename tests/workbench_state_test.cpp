#include "browser/workbench_state.h"
#include "test_check.h"

#include <chrono>
#include <string>

namespace {
  using luciole::Location;
  using luciole::WorkbenchEntry;
  using luciole::WorkbenchHover;
  using luciole::WorkbenchPane;
  using luciole::WorkbenchState;
  using luciole::WorkbenchTargetSource;
  using Time = WorkbenchState::Clock::time_point;
  using std::chrono::milliseconds;

  WorkbenchEntry folder(const std::string& name) {
    return {.location = Location::fromPath("/workbench-state/" + name), .folder = true};
  }
  WorkbenchEntry file(const std::string& name) {
    return {.location = Location::fromPath("/workbench-state/" + name), .folder = false};
  }
  Time at(int ms) { return Time{} + milliseconds(ms); }

  // A state whose cursor is on "cursor" and whose pointer rested on `entry` in the list at time 0.
  WorkbenchState restedOn(const WorkbenchEntry& entry) {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(entry, 10, 10, at(0));
    state.advance(at(250));
    TEST_CHECK(state.target() == entry);
    return state;
  }

  void testStartsEmpty() {
    const WorkbenchState state;
    TEST_CHECK(state.serial() == 0);
    TEST_CHECK(!state.target());
    TEST_CHECK(state.targetSource() == WorkbenchTargetSource::Cursor);
    TEST_CHECK(!state.hovered());
    TEST_CHECK(!state.expanded());
    TEST_CHECK(!state.locked());
    TEST_CHECK(!state.pinned(0) && !state.pinned(1));
    TEST_CHECK(!state.deadline());
  }

  void testCursorIsTheTargetUntilThePointerRests() {
    WorkbenchState state;
    state.setCursor(folder("a"));
    TEST_CHECK(state.target() == folder("a"));
    TEST_CHECK(state.targetSource() == WorkbenchTargetSource::Cursor);
    const auto serial = state.serial();
    TEST_CHECK(serial > 0);
    state.setCursor(folder("a"));
    TEST_CHECK(state.serial() == serial);
    state.setCursor(std::nullopt);
    TEST_CHECK(!state.target());
    TEST_CHECK(state.serial() > serial);
  }

  void testRestingRetargets() {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(file("f.txt"), 10, 10, at(0));
    TEST_CHECK(state.deadline() == at(250));
    const auto serial = state.serial();
    state.advance(at(249));
    TEST_CHECK(state.target() == folder("cursor"));
    TEST_CHECK(state.serial() == serial);
    state.advance(at(250));
    TEST_CHECK(state.target() == file("f.txt"));
    TEST_CHECK(state.targetSource() == WorkbenchTargetSource::Pointer);
    TEST_CHECK(state.serial() > serial);
    TEST_CHECK(!state.deadline());
  }

  void testSmallMovesKeepTheWaitAndLargeOnesRestartIt() {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(folder("x"), 10, 10, at(0));
    state.pointerInList(folder("x"), 14, 14, at(200)); // 5.7 px from where the wait began
    state.advance(at(250));
    TEST_CHECK(state.target() == folder("x"));

    WorkbenchState moved;
    moved.setCursor(folder("cursor"));
    moved.pointerInList(folder("x"), 10, 10, at(0));
    moved.pointerInList(folder("x"), 17, 10, at(200)); // 7 px: the wait starts again
    moved.advance(at(449));
    TEST_CHECK(moved.target() == folder("cursor"));
    moved.advance(at(450));
    TEST_CHECK(moved.target() == folder("x"));
  }

  void testAnotherRowRestartsTheWait() {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(folder("x"), 10, 10, at(0));
    state.pointerInList(folder("y"), 10, 12, at(200));
    state.advance(at(300));
    TEST_CHECK(state.target() == folder("cursor"));
    state.advance(at(450));
    TEST_CHECK(state.target() == folder("y"));
  }

  void testRestingOnTheTargetWaitsForNothing() {
    auto state = restedOn(folder("x"));
    state.pointerInList(folder("x"), 40, 40, at(300));
    TEST_CHECK(!state.deadline());
  }

  void testEventsApplyWhatIsDueFirst() {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(folder("x"), 10, 10, at(0));
    state.pointerInList(folder("y"), 30, 10, at(260)); // x rested 260 ms before the pointer moved on
    TEST_CHECK(state.target() == folder("x"));
    state.advance(at(510));
    TEST_CHECK(state.target() == folder("y"));
  }

  void testLeavingFallsBackToTheCursor() {
    auto state = restedOn(folder("x"));
    state.pointerLeft(at(1000));
    TEST_CHECK(state.deadline() == at(1300));
    state.pointerLeft(at(1200)); // still outside: the release keeps its time
    state.advance(at(1299));
    TEST_CHECK(state.target() == folder("x"));
    state.advance(at(1300));
    TEST_CHECK(state.target() == folder("cursor"));
    TEST_CHECK(state.targetSource() == WorkbenchTargetSource::Cursor);
  }

  void testColumnsAreInsideTheArea() {
    auto state = restedOn(folder("x"));
    state.pointerLeft(at(1000));
    state.pointerInColumn(0, std::nullopt, 400, 10, at(1100));
    TEST_CHECK(!state.deadline());
    state.advance(at(5000));
    TEST_CHECK(state.target() == folder("x"));
  }

  void testEmptyListSpaceIsLeaving() {
    auto state = restedOn(folder("x"));
    state.pointerInList(folder("y"), 10, 40, at(400));
    state.pointerInList(std::nullopt, 10, 900, at(500));
    TEST_CHECK(state.deadline() == at(800));
    state.advance(at(800));
    TEST_CHECK(state.target() == folder("cursor"));
  }

  void testRestingInColumnsSteersTheNextColumn() {
    auto state = restedOn(folder("x"));
    state.pointerInColumn(0, folder("x/b"), 400, 10, at(300));
    state.advance(at(549));
    TEST_CHECK(!state.pinned(0));
    state.advance(at(550));
    TEST_CHECK(state.pinned(0) == folder("x/b"));
    TEST_CHECK(!state.locked());
    const auto steered = state.serial();
    state.pointerInColumn(0, folder("x/b"), 420, 30, at(560)); // resting on the pinned row waits for nothing
    TEST_CHECK(!state.deadline());
    TEST_CHECK(state.serial() == steered);
    state.pointerInColumn(1, file("x/b/c.txt"), 700, 10, at(600));
    state.advance(at(850));
    TEST_CHECK(state.pinned(1) == file("x/b/c.txt"));
    state.pointerInColumn(2, folder("x/b/d"), 900, 10, at(900)); // the last column steers nothing
    TEST_CHECK(!state.deadline());
    state.pointerInColumn(0, folder("x/e"), 400, 30, at(1000));
    state.advance(at(1250));
    TEST_CHECK(state.pinned(0) == folder("x/e"));
    TEST_CHECK(!state.pinned(1)); // it belonged to the column that changed
    TEST_CHECK(state.target() == folder("x"));
  }

  void testRestingInTheListClearsSteering() {
    auto state = restedOn(folder("x"));
    state.pointerInColumn(0, folder("x/b"), 400, 10, at(300));
    state.advance(at(550));
    state.pointerInList(folder("y"), 10, 40, at(600));
    state.advance(at(850));
    TEST_CHECK(state.target() == folder("y"));
    TEST_CHECK(!state.pinned(0));
  }

  void testClickPinsUntilADeliberateAction() {
    auto state = restedOn(folder("x"));
    state.pin(0, file("x/a.txt"));
    TEST_CHECK(state.pinned(0) == file("x/a.txt"));
    TEST_CHECK(state.locked());
    state.pointerInList(folder("y"), 10, 40, at(300));
    state.advance(at(600));
    TEST_CHECK(state.target() == folder("x")); // the pointer waits
    state.pointerLeft(at(700));
    state.advance(at(1100));
    TEST_CHECK(state.target() == folder("x"));
    const auto serial = state.serial();
    state.unpin();
    TEST_CHECK(state.serial() > serial);
    TEST_CHECK(!state.locked());
    TEST_CHECK(!state.pinned(0));
    state.pin(2, file("x/b/c/d.txt")); // the last column has no slot after it
    TEST_CHECK(!state.locked());
    TEST_CHECK(!state.pinned(2));
  }

  void testPinInColumnTwoHoldsItsFolder() {
    auto state = restedOn(folder("x"));
    state.pin(1, file("x/b/c.txt"));
    TEST_CHECK(state.pinned(0) == folder("x/b"));
    TEST_CHECK(state.pinned(1) == file("x/b/c.txt"));
    TEST_CHECK(state.locked());

    auto rested = restedOn(folder("x"));
    rested.pointerInColumn(1, folder("x/d/e"), 700, 10, at(300));
    rested.advance(at(550));
    TEST_CHECK(rested.pinned(0) == folder("x/d"));
    TEST_CHECK(rested.pinned(1) == folder("x/d/e"));
    TEST_CHECK(!rested.locked());
  }

  void testFrozenWhileEditing() {
    auto state = restedOn(folder("x"));
    state.setFrozen(true);
    state.pointerInList(folder("y"), 10, 40, at(300));
    state.advance(at(600));
    TEST_CHECK(state.target() == folder("x"));
    state.pointerLeft(at(700));
    state.advance(at(1100));
    TEST_CHECK(state.target() == folder("x"));
    state.setFrozen(false);
    state.pointerInList(folder("y"), 10, 40, at(1200));
    state.advance(at(1450));
    TEST_CHECK(state.target() == folder("y"));
  }

  // While frozen the pointer starts no wait and no release, so nothing is due and nothing wakes.
  void testFrozenStartsNoWaits() {
    auto state = restedOn(folder("x"));
    state.pointerInList(folder("y"), 10, 40, at(300));
    TEST_CHECK(state.deadline().has_value());
    const auto serial = state.serial();
    state.setFrozen(true);
    TEST_CHECK(state.serial() != serial);
    TEST_CHECK(!state.deadline()); // the wait that began before is dropped
    state.pointerInList(folder("z"), 10, 80, at(400));
    TEST_CHECK(!state.deadline());
    state.pointerInList(std::nullopt, 10, 400, at(500));
    TEST_CHECK(!state.deadline());
    state.pointerInColumn(0, file("x/a.txt"), 500, 40, at(600));
    TEST_CHECK(!state.deadline());
    state.pointerLeft(at(700));
    TEST_CHECK(!state.deadline());
    state.advance(at(5000));
    TEST_CHECK(state.target() == folder("x") && !state.pinned(0));
    // A deliberate move of the cursor still retargets.
    state.setCursor(file("other.txt"));
    TEST_CHECK(state.target() == file("other.txt"));
    TEST_CHECK(state.targetSource() == WorkbenchTargetSource::Cursor);
    state.setFrozen(false);
    state.pointerInList(folder("y"), 10, 40, at(5100));
    TEST_CHECK(state.deadline().has_value());
  }

  // While the cursor is held, a cursor that goes away without a new one keeps the target: a save that replaces the
  // edited file takes the list's cursor with it until the page puts it back. The end of the hold lets a held cursor
  // go, and so does a folder change.
  void testHeldCursor() {
    WorkbenchState state;
    state.setCursor(file("notes.txt"));
    state.holdCursor(true);
    auto serial = state.serial();
    state.setCursor(std::nullopt);
    TEST_CHECK(state.target() == file("notes.txt") && state.serial() == serial);
    state.setCursor(file("notes.txt")); // the list holds the file again
    TEST_CHECK(state.target() == file("notes.txt") && state.serial() == serial);
    state.setCursor(file("other.txt"));
    TEST_CHECK(state.target() == file("other.txt"));
    state.setCursor(std::nullopt);
    TEST_CHECK(state.target() == file("other.txt")); // a new cursor is held too
    serial = state.serial();
    state.holdCursor(false);
    TEST_CHECK(!state.target() && state.serial() != serial); // the held cursor goes with the hold
    state.setCursor(file("notes.txt"));
    state.setCursor(std::nullopt);
    TEST_CHECK(!state.target()); // no hold: the cursor goes

    state.setFrozen(true); // freezing holds the pointer, not the cursor
    state.setCursor(file("notes.txt"));
    state.setCursor(std::nullopt);
    TEST_CHECK(!state.target());
    state.setFrozen(false);

    state.holdCursor(true);
    state.setCursor(file("notes.txt"));
    state.setCursor(std::nullopt);
    state.reset(); // another folder
    TEST_CHECK(!state.target());
    state.holdCursor(false);
    TEST_CHECK(!state.target());
  }

  void testCursorMoveForgetsPointerPinsAndTimers() {
    auto state = restedOn(folder("x"));
    state.pin(0, file("x/a.txt"));
    state.pointerInList(folder("y"), 10, 40, at(300));
    state.setCursor(folder("other"));
    TEST_CHECK(state.target() == folder("other"));
    TEST_CHECK(!state.pinned(0));
    TEST_CHECK(!state.locked());
    TEST_CHECK(!state.deadline());
  }

  void testHoverBelongsToItsPane() {
    WorkbenchState state;
    state.hover(WorkbenchPane::List, folder("x"));
    TEST_CHECK((state.hovered() == WorkbenchHover{.entry = folder("x"), .pane = WorkbenchPane::List}));
    auto serial = state.serial();
    state.hover(WorkbenchPane::List, folder("x"));
    TEST_CHECK(state.serial() == serial);
    state.unhover(WorkbenchPane::Instrument);
    TEST_CHECK(state.hovered().has_value());
    state.unhover(WorkbenchPane::List);
    TEST_CHECK(!state.hovered());
    // One column entered before the other one left.
    state.hover(WorkbenchPane::ColumnA, folder("x/a"));
    state.hover(WorkbenchPane::ColumnB, folder("x/a/b"));
    serial = state.serial();
    state.unhover(WorkbenchPane::ColumnA);
    TEST_CHECK(state.hovered()->pane == WorkbenchPane::ColumnB);
    TEST_CHECK(state.serial() == serial);
    state.hover(WorkbenchPane::Instrument, folder("x/a/b"));
    TEST_CHECK(state.hovered()->pane == WorkbenchPane::Instrument);
    TEST_CHECK(state.serial() > serial);
  }

  void testResetForgetsEverythingButCursorAndExpansion() {
    auto state = restedOn(folder("x"));
    state.hover(WorkbenchPane::Ribbon, folder("x"));
    state.pin(0, file("x/a.txt"));
    state.setExpanded(true);
    state.pointerLeft(at(400));
    state.reset();
    TEST_CHECK(state.target() == folder("cursor"));
    TEST_CHECK(!state.hovered());
    TEST_CHECK(!state.pinned(0));
    TEST_CHECK(!state.locked());
    TEST_CHECK(!state.deadline());
    TEST_CHECK(state.expanded());
  }

  void testExpandedFlag() {
    WorkbenchState state;
    state.setExpanded(true);
    TEST_CHECK(state.expanded());
    const auto serial = state.serial();
    state.setExpanded(true);
    TEST_CHECK(state.serial() == serial);
    state.setExpanded(false);
    TEST_CHECK(!state.expanded());
    TEST_CHECK(state.serial() > serial);
  }

  void testTimeGoingBackFiresNothing() {
    WorkbenchState state;
    state.setCursor(folder("cursor"));
    state.pointerInList(folder("x"), 10, 10, at(1000));
    state.advance(at(0));
    TEST_CHECK(state.target() == folder("cursor"));
    TEST_CHECK(state.deadline() == at(1250));
  }
} // namespace

int main() {
  testStartsEmpty();
  testCursorIsTheTargetUntilThePointerRests();
  testRestingRetargets();
  testSmallMovesKeepTheWaitAndLargeOnesRestartIt();
  testAnotherRowRestartsTheWait();
  testRestingOnTheTargetWaitsForNothing();
  testEventsApplyWhatIsDueFirst();
  testLeavingFallsBackToTheCursor();
  testColumnsAreInsideTheArea();
  testEmptyListSpaceIsLeaving();
  testRestingInColumnsSteersTheNextColumn();
  testRestingInTheListClearsSteering();
  testClickPinsUntilADeliberateAction();
  testPinInColumnTwoHoldsItsFolder();
  testFrozenWhileEditing();
  testFrozenStartsNoWaits();
  testHeldCursor();
  testCursorMoveForgetsPointerPinsAndTimers();
  testHoverBelongsToItsPane();
  testResetForgetsEverythingButCursorAndExpansion();
  testExpandedFlag();
  testTimeGoingBackFiresNothing();
  return 0;
}
