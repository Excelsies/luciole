#include "editing/table_session.h"
#include "test_check.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::editing;
  using luciole::formats::Delimiter;
  using luciole::formats::Dialect;

  // Processor time one change on a 1 MiB file of 20,000 rows may take; about 15 ms at -O0.
  constexpr double kChangeSecondsBound = 0.1;

  // A plain sentence: starts with a capital and ends with a period.
  bool isSentence(std::string_view text) {
    return text.size() > 2 && text.front() >= 'A' && text.front() <= 'Z' && text.back() == '.';
  }

  TableRefusal openRefusal(std::string bytes, const Dialect& dialect = {}) {
    const auto session = TableSession::open(std::move(bytes), dialect);
    TEST_CHECK(!session.has_value());
    TEST_CHECK(isSentence(session.error().message));
    return session.error().reason;
  }

  // `rows` lines of `width` bytes each, the line ending included.
  std::string fixedRows(std::size_t rows, std::size_t width) {
    std::string line(width - 1, 'x');
    line.push_back('\n');
    std::string bytes;
    bytes.reserve(rows * width);
    for (std::size_t i = 0; i < rows; ++i) {
      bytes += line;
    }
    return bytes;
  }

  void testOpenReadsTheFile() {
    const std::string bytes = "name,qty\n\"a,b\",2\nc\n";
    const Dialect dialect{.header = true, .finalNewline = true};
    auto session = TableSession::open(bytes, dialect);
    TEST_CHECK(session.has_value());
    TEST_CHECK(session->bytes() == bytes);
    TEST_CHECK(session->dialect() == dialect);
    TEST_CHECK(session->rowCount() == 3);
    TEST_CHECK(session->columnCount() == 2);
    TEST_CHECK(session->table().rows.size() == 3);
    TEST_CHECK(session->table().cells.size() == 5);
    TEST_CHECK(session->revision() == 0);
    TEST_CHECK(!session->modified());
  }

  void testOpenSizeLimit() {
    // 16384 lines of 64 bytes are exactly 1 MiB.
    std::string bytes = fixedRows(16384, 64);
    TEST_CHECK(bytes.size() == 1'048'576);
    TEST_CHECK(TableSession::open(bytes, Dialect{}).has_value());
    bytes.push_back('x');
    TEST_CHECK(openRefusal(bytes) == TableRefusal::TooLarge);
  }

  void testOpenRefusesTruncatedTables() {
    // 100,001 empty lines pass the reader's row limit.
    TEST_CHECK(openRefusal(std::string(100'001, '\n')) == TableRefusal::Truncated);
    TEST_CHECK(TableSession::open(std::string(100'000, '\n'), Dialect{}).has_value());
    // 1001 cells pass the column limit.
    TEST_CHECK(openRefusal(std::string(1000, ',') + "\n") == TableRefusal::Truncated);
    TEST_CHECK(TableSession::open(std::string(999, ',') + "\n", Dialect{}).has_value());
    // 1001 rows of 1000 cells pass the cell limit of 1,000,000.
    std::string cells;
    for (std::size_t i = 0; i < 1001; ++i) {
      cells += std::string(999, ',') + "\n";
    }
    TEST_CHECK(cells.size() == 1'001'000);
    TEST_CHECK(openRefusal(cells) == TableRefusal::Truncated);
  }

  void testOpenRefusesUnterminatedQuote() {
    TEST_CHECK(openRefusal("a,\"b\nc") == TableRefusal::UnterminatedQuote);
    TEST_CHECK(
        openRefusal("a\t'b\n", Dialect{.delimiter = Delimiter::Tab, .quote = luciole::formats::Quote::Single})
        == TableRefusal::UnterminatedQuote
    );
  }

  void testOpenEmptyFile() {
    auto session = TableSession::open("", Dialect{});
    TEST_CHECK(session.has_value());
    TEST_CHECK(session->rowCount() == 0 && session->columnCount() == 0);
  }

  TableSession opened(std::string bytes, const Dialect& dialect = {}) {
    auto session = TableSession::open(std::move(bytes), dialect);
    TEST_CHECK(session.has_value());
    return std::move(*session);
  }

  void accepted(const std::expected<void, TableError>& result) { TEST_CHECK(result.has_value()); }

  // How many undo steps the history holds, leaving the session at the oldest position.
  std::size_t undoAll(TableSession& session) {
    std::size_t steps = 0;
    while (steps < 1000 && session.undo()) {
      ++steps;
    }
    return steps;
  }

  void testUndoRedoEachChange() {
    const std::string base = "a,b\n1,2\n";
    struct Case {
      std::expected<void, TableError> (*change)(TableSession&);
      std::string after;
      std::size_t rows;
      std::size_t columns;
    };
    const std::vector<Case> cases{
        {[](TableSession& s) { return s.setCell(1, 1, "x"); }, "a,b\n1,x\n", 2, 2},
        {[](TableSession& s) { return s.insertRow(1); }, "a,b\n,\n1,2\n", 3, 2},
        {[](TableSession& s) { return s.deleteRow(0); }, "1,2\n", 1, 2},
        {[](TableSession& s) { return s.insertColumn(1); }, "a,,b\n1,,2\n", 2, 3},
        {[](TableSession& s) { return s.deleteColumn(0); }, "b\n2\n", 2, 1},
    };
    for (const Case& item : cases) {
      auto session = opened(base);
      TEST_CHECK(!session.canUndo() && !session.canRedo());
      accepted(item.change(session));
      const std::uint64_t changed = session.revision();
      TEST_CHECK(changed > 0 && session.bytes() == item.after);
      TEST_CHECK(session.rowCount() == item.rows && session.columnCount() == item.columns);
      TEST_CHECK(session.canUndo() && !session.canRedo());
      TEST_CHECK(session.undo());
      TEST_CHECK(session.bytes() == base && session.revision() == 0);
      TEST_CHECK(session.rowCount() == 2 && session.columnCount() == 2 && session.table().cells.size() == 4);
      TEST_CHECK(!session.canUndo() && session.canRedo());
      TEST_CHECK(!session.undo());
      TEST_CHECK(session.redo());
      TEST_CHECK(session.bytes() == item.after && session.revision() == changed);
      TEST_CHECK(session.rowCount() == item.rows && session.columnCount() == item.columns);
      TEST_CHECK(session.canUndo() && !session.canRedo());
      TEST_CHECK(!session.redo());
    }
  }

  void testChangeAfterUndoDropsRedo() {
    auto session = opened("a,b\n");
    accepted(session.setCell(0, 0, "x"));
    accepted(session.setCell(0, 1, "y"));
    TEST_CHECK(session.undo());
    TEST_CHECK(session.canUndo() && session.canRedo());
    accepted(session.setCell(0, 0, "z"));
    TEST_CHECK(session.bytes() == "z,b\n");
    TEST_CHECK(session.canUndo() && !session.canRedo());
    TEST_CHECK(!session.redo());
    TEST_CHECK(session.undo() && session.bytes() == "x,b\n");
    TEST_CHECK(session.canUndo() && session.canRedo());
    TEST_CHECK(session.undo() && session.bytes() == "a,b\n");
    TEST_CHECK(!session.canUndo() && session.canRedo());
    TEST_CHECK(!session.undo());
  }

  void testHistoryStepBound() {
    auto session = opened("a,b\n");
    TEST_CHECK(!session.canUndo() && !session.canRedo());
    for (std::size_t i = 0; i < kUndoStepLimit + 5; ++i) {
      accepted(session.setCell(0, 0, "v" + std::to_string(i)));
      TEST_CHECK(session.canUndo() && !session.canRedo());
    }
    // 106 positions, of which the newest 101 are kept: the oldest left is the change that wrote v4.
    std::size_t undone = 0;
    while (undone < kUndoStepLimit + 10 && session.canUndo()) {
      TEST_CHECK(session.undo());
      ++undone;
      TEST_CHECK(session.canRedo());
    }
    TEST_CHECK(undone == kUndoStepLimit);
    TEST_CHECK(!session.canUndo() && session.canRedo() && !session.undo());
    TEST_CHECK(session.bytes() == "v4,b\n");
    std::size_t redone = 0;
    while (redone < kUndoStepLimit + 10 && session.canRedo()) {
      TEST_CHECK(session.redo());
      ++redone;
      TEST_CHECK(session.canUndo());
    }
    TEST_CHECK(redone == kUndoStepLimit);
    TEST_CHECK(session.canUndo() && !session.canRedo() && !session.redo());
    TEST_CHECK(session.bytes() == "v104,b\n");
  }

  // Writes 63 copies of each letter from `first` to `last` into row 0, one change per letter.
  void writeLetters(TableSession& session, char first, char last) {
    for (char letter = first; letter <= last; ++letter) {
      accepted(session.setCell(0, 0, std::string(63, letter)));
    }
  }

  void testHistoryByteBound() {
    // Snapshots of exactly 1 MiB: 16 of them are exactly 16 MiB and fit, 17 do not.
    const std::string bytes = fixedRows(16384, 64);
    auto session = opened(bytes);
    writeLetters(session, 'A', 'Y');
    TEST_CHECK(session.bytes().front() == 'Y');
    // 26 positions, of which the newest 16 are kept: the oldest left is the 10th change, which wrote J.
    TEST_CHECK(undoAll(session) == 15);
    TEST_CHECK(session.bytes().front() == 'J' && session.bytes().size() == kTableByteLimit);
  }

  void testHistoryBytesAfterRedoIsDropped() {
    // The bytes of the positions a change drops no longer count against the bound.
    const std::string bytes = fixedRows(16384, 64);
    auto session = opened(bytes);
    writeLetters(session, 'a', 'j');
    TEST_CHECK(undoAll(session) == 10 && session.bytes() == bytes);
    writeLetters(session, 'A', 'O');
    TEST_CHECK(undoAll(session) == 15);
    TEST_CHECK(session.bytes() == bytes);
  }

  // A refused change leaves the redo steps alone: `redone` is still one redo away.
  void checkRedoKept(TableSession& session, const std::expected<void, TableError>& result, std::string_view redone) {
    TEST_CHECK(!result.has_value());
    TEST_CHECK(session.redo() && session.bytes() == redone);
    TEST_CHECK(session.undo());
  }

  void testRefusedChangeKeepsRedo() {
    auto small = opened("a,b\n1,2\n");
    accepted(small.setCell(0, 0, "x"));
    TEST_CHECK(small.undo());
    checkRedoKept(small, small.setCell(9, 0, "y"), "x,b\n1,2\n");
    checkRedoKept(small, small.insertRow(9), "x,b\n1,2\n");
    checkRedoKept(small, small.deleteRow(9), "x,b\n1,2\n");
    checkRedoKept(small, small.insertColumn(9), "x,b\n1,2\n");
    checkRedoKept(small, small.deleteColumn(9), "x,b\n1,2\n");

    auto single = opened("a\n");
    accepted(single.setCell(0, 0, "x"));
    TEST_CHECK(single.undo());
    checkRedoKept(single, single.deleteRow(0), "x\n");
    checkRedoKept(single, single.deleteColumn(0), "x\n");

    // A single column of exactly 1 MiB: an inserted row would pass the byte limit.
    const std::string full = fixedRows(16384, 64);
    auto large = opened(full);
    accepted(large.setCell(0, 0, std::string(63, 'y')));
    const std::string changed = large.bytes();
    TEST_CHECK(large.undo());
    checkRedoKept(large, large.insertRow(0), changed);

    // 1000 cells are the reader's column limit.
    auto wide = opened(std::string(999, ',') + "\n");
    accepted(wide.setCell(0, 0, "x"));
    TEST_CHECK(wide.undo());
    checkRedoKept(wide, wide.insertColumn(0), "x" + std::string(999, ',') + "\n");
  }

  void testModifiedThroughSaveAndUndo() {
    auto session = opened("a,b\n");
    TEST_CHECK(!session.modified());
    accepted(session.setCell(0, 0, "x"));
    TEST_CHECK(session.modified());
    session.markSaved(session.revision());
    TEST_CHECK(!session.modified());
    // The same text again is no change.
    accepted(session.setCell(0, 0, "x"));
    TEST_CHECK(!session.modified());
    TEST_CHECK(session.undo() && session.modified());
    TEST_CHECK(session.redo() && !session.modified());
    accepted(session.setCell(0, 1, "y"));
    TEST_CHECK(session.modified());
    TEST_CHECK(session.undo() && !session.modified());
  }

  void testUndoBackToOpenIsClean() {
    auto session = opened("a,b\n");
    accepted(session.setCell(0, 0, "x"));
    accepted(session.insertRow(0));
    TEST_CHECK(session.undo() && session.modified());
    TEST_CHECK(session.undo() && !session.modified());
    TEST_CHECK(session.redo() && session.modified());
  }

  void testSaveFinishedAfterAnotherChange() {
    auto session = opened("a,b\n");
    accepted(session.setCell(0, 0, "x"));
    const std::uint64_t written = session.revision();
    accepted(session.setCell(0, 1, "y"));
    session.markSaved(written);
    TEST_CHECK(session.modified());
    TEST_CHECK(session.undo() && !session.modified());
    TEST_CHECK(session.undo() && session.modified());
    TEST_CHECK(session.redo() && !session.modified());
  }

  void testSaveOfUnreachableRevision() {
    // The written revision was dropped by a change after undo: no position matches the file.
    auto session = opened("a,b\n");
    accepted(session.setCell(0, 0, "x"));
    const std::uint64_t written = session.revision();
    TEST_CHECK(session.undo());
    accepted(session.setCell(0, 0, "z"));
    session.markSaved(written);
    TEST_CHECK(session.modified());
    TEST_CHECK(session.undo() && session.modified());

    // The written revision fell off the end of the history.
    auto old = opened("a,b\n");
    for (std::size_t i = 0; i < 101; ++i) {
      accepted(old.setCell(0, 0, "v" + std::to_string(i)));
    }
    old.markSaved(0);
    TEST_CHECK(old.modified());
    TEST_CHECK(undoAll(old) == 100 && old.modified());
  }

  void testRevisionsAreNeverReused() {
    auto session = opened("a,b\n");
    accepted(session.setCell(0, 0, "x"));
    const std::uint64_t first = session.revision();
    TEST_CHECK(session.undo());
    accepted(session.setCell(0, 0, "x"));
    TEST_CHECK(session.revision() > first);
    // The dialect stays the one given at open.
    const Dialect header{.header = true};
    auto fixed = opened("a,b\n1,2\n", header);
    accepted(fixed.insertColumn(0));
    TEST_CHECK(fixed.dialect() == header);
  }

  double secondsSince(std::clock_t start) {
    return static_cast<double>(std::clock() - start) / static_cast<double>(CLOCKS_PER_SEC);
  }

  void testSpeedOnLargeFile() {
    // 20,000 rows of 52 bytes, five cells each.
    std::string bytes = "id,name,code,pair,note\n";
    for (std::size_t i = 1; i < 20'000; ++i) {
      bytes += std::format("{:05},item {:05},{:010},\"a, b\",sixteen chars ok\n", i, i, i * 7);
    }
    TEST_CHECK(bytes.size() > 1'000'000 && bytes.size() <= kTableByteLimit);
    auto session = opened(std::move(bytes), Dialect{.header = true, .finalNewline = true});
    TEST_CHECK(session.rowCount() == 20'000 && session.columnCount() == 5);

    std::clock_t start = std::clock();
    accepted(session.setCell(10'000, 4, "changed, with a comma"));
    const double setCellSeconds = secondsSince(start);
    start = std::clock();
    accepted(session.insertRow(10'000));
    const double insertRowSeconds = secondsSince(start);
    std::println("setCell {:.4f} s, insertRow {:.4f} s of processor time", setCellSeconds, insertRowSeconds);
    TEST_CHECK(session.rowCount() == 20'001);
    TEST_CHECK(setCellSeconds < kChangeSecondsBound);
    TEST_CHECK(insertRowSeconds < kChangeSecondsBound);
  }
} // namespace

int main() {
  testOpenReadsTheFile();
  testOpenSizeLimit();
  testOpenRefusesTruncatedTables();
  testOpenRefusesUnterminatedQuote();
  testOpenEmptyFile();
  testUndoRedoEachChange();
  testChangeAfterUndoDropsRedo();
  testHistoryStepBound();
  testHistoryByteBound();
  testHistoryBytesAfterRedoIsDropped();
  testRefusedChangeKeepsRedo();
  testModifiedThroughSaveAndUndo();
  testUndoBackToOpenIsClean();
  testSaveFinishedAfterAnotherChange();
  testSaveOfUnreachableRevision();
  testRevisionsAreNeverReused();
  testSpeedOnLargeFile();
  std::println("editing table session tests passed");
  return 0;
}
