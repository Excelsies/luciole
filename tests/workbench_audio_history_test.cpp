#include "test_check.h"
#include "workbench/audio_history.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <print>
#include <stdexcept>

namespace {
  using luciole::workbench::AudioHistory;
  using luciole::workbench::HistoryChange;

  void startsDarkAndWhole() {
    AudioHistory history(8, 4);
    TEST_CHECK(history.width() == 8);
    TEST_CHECK(history.rows() == 4);
    TEST_CHECK(history.texels().size() == 8 * 4 * 4);
    TEST_CHECK(std::ranges::all_of(history.texels(), [](std::uint8_t byte) { return byte == 0; }));
    // A new history has never been uploaded.
    TEST_CHECK(history.takeUpdate().change == HistoryChange::All);
    TEST_CHECK(history.takeUpdate().change == HistoryChange::None);
  }

  void oneWrittenRowIsTheUpdate() {
    AudioHistory history(3, 5);
    (void)history.takeUpdate();
    const auto row = history.writeRow(2);
    TEST_CHECK(row.size() == 12);
    row[0] = 7;
    row[11] = 9;
    const auto update = history.takeUpdate();
    TEST_CHECK(update.change == HistoryChange::Row);
    TEST_CHECK(update.row == 2);
    TEST_CHECK(history.texels()[24] == 7);
    TEST_CHECK(history.texels()[35] == 9);
    TEST_CHECK(history.texels()[23] == 0);
    TEST_CHECK(history.texels()[36] == 0);
    TEST_CHECK(history.takeUpdate().change == HistoryChange::None);
  }

  void theSameRowTwiceIsStillOneRow() {
    AudioHistory history(2, 2);
    (void)history.takeUpdate();
    history.writeRow(1)[0] = 1;
    history.writeRow(1)[0] = 2;
    const auto update = history.takeUpdate();
    TEST_CHECK(update.change == HistoryChange::Row && update.row == 1);
  }

  void twoRowsMeanEverything() {
    AudioHistory history(2, 3);
    (void)history.takeUpdate();
    history.writeRow(0)[0] = 1;
    history.writeRow(2)[0] = 1;
    TEST_CHECK(history.takeUpdate().change == HistoryChange::All);
    TEST_CHECK(history.takeUpdate().change == HistoryChange::None);
  }

  void clearDarkensEverything() {
    AudioHistory history(2, 2);
    history.writeRow(0)[3] = 200;
    (void)history.takeUpdate();
    history.clear();
    TEST_CHECK(std::ranges::all_of(history.texels(), [](std::uint8_t byte) { return byte == 0; }));
    TEST_CHECK(history.takeUpdate().change == HistoryChange::All);
  }

  void refusesBadSizesAndRows() {
    bool refused = false;
    try {
      AudioHistory history(0, 4);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
    refused = false;
    try {
      AudioHistory history(4, AudioHistory::kMaxSide + 1);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
    AudioHistory history(4, 4);
    (void)history.takeUpdate();
    refused = false;
    try {
      (void)history.writeRow(4);
    } catch (const std::out_of_range&) {
      refused = true;
    }
    TEST_CHECK(refused);
    TEST_CHECK(history.takeUpdate().change == HistoryChange::None);
  }
} // namespace

int main() {
  startsDarkAndWhole();
  oneWrittenRowIsTheUpdate();
  theSameRowTwiceIsStillOneRow();
  twoRowsMeanEverything();
  clearDarkensEverything();
  refusesBadSizesAndRows();
  std::println("audio history checks passed");
  return 0;
}
