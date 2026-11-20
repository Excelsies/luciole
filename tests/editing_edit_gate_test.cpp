#include "editing/edit_gate.h"
#include "test_check.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <print>
#include <string>

namespace {
  using luciole::editing::Answer;
  using luciole::editing::EditGate;
  using luciole::editing::GateResult;
  using luciole::editing::Notice;
  using luciole::editing::Proceed;
  using luciole::editing::Refusal;
  using luciole::editing::SaveSucceeded;

  void testEnableNeedsEditableFile() {
    EditGate gate;
    TEST_CHECK(!gate.editing() && !gate.dirty() && !gate.saving() && !gate.asking() && !gate.saveEnabled());
    const auto refused = gate.enable(false);
    TEST_CHECK(refused.refusal == Refusal::NotEditable);
    TEST_CHECK(!gate.editing());
    const auto accepted = gate.enable(true);
    TEST_CHECK(accepted.refusal == Refusal::None);
    TEST_CHECK(gate.editing() && !gate.dirty() && !gate.saveEnabled());
  }

  void testEnableWhileEditingIsRefused() {
    EditGate gate;
    (void)gate.enable(true);
    TEST_CHECK(gate.enable(true).refusal == Refusal::AlreadyEditing);
    // Already editing is checked before the editable flag.
    TEST_CHECK(gate.enable(false).refusal == Refusal::AlreadyEditing);
    TEST_CHECK(gate.editing());
  }

  void testSaveNeedsChanges() {
    EditGate gate;
    TEST_CHECK(gate.save().refusal == Refusal::NotEditing);
    TEST_CHECK(gate.setDirty(true).refusal == Refusal::NotEditing && !gate.dirty());
    (void)gate.enable(true);
    const auto clean = gate.save();
    TEST_CHECK(clean.refusal == Refusal::NothingToSave && !clean.startSave && !gate.saving());
    TEST_CHECK(gate.setDirty(true).refusal == Refusal::None);
    TEST_CHECK(gate.dirty() && gate.saveEnabled());
    const auto started = gate.save();
    TEST_CHECK(started.refusal == Refusal::None && started.startSave);
    TEST_CHECK(gate.saving() && !gate.saveEnabled());
    const auto again = gate.save();
    TEST_CHECK(again.refusal == Refusal::SaveRunning && !again.startSave && gate.saving());
  }

  EditGate savingGate() {
    EditGate gate;
    (void)gate.enable(true);
    (void)gate.setDirty(true);
    (void)gate.save();
    return gate;
  }

  void testSaveFinished() {
    auto clean = savingGate();
    const auto done = clean.saveFinished(SaveSucceeded{.changedSinceStart = false});
    TEST_CHECK(done.refusal == Refusal::None && done.notice == Notice::None);
    TEST_CHECK(clean.editing() && !clean.dirty() && !clean.saving() && !clean.saveEnabled());

    auto typed = savingGate();
    TEST_CHECK(typed.saveFinished(SaveSucceeded{.changedSinceStart = true}).refusal == Refusal::None);
    TEST_CHECK(typed.editing() && typed.dirty() && !typed.saving() && typed.saveEnabled());

    auto failed = savingGate();
    const auto failure = failed.saveFinished(std::unexpected(std::string("The disk is full.")));
    TEST_CHECK(failure.refusal == Refusal::None && failure.notice == Notice::SaveFailed);
    TEST_CHECK(failure.sentence == "The disk is full." && !failure.closeEditor && !failure.discardText);
    TEST_CHECK(failed.editing() && failed.dirty() && !failed.saving() && failed.saveEnabled());
  }

  void testSaveFinishedRefusals() {
    EditGate idle;
    (void)idle.enable(true);
    (void)idle.setDirty(true);
    TEST_CHECK(idle.saveFinished(SaveSucceeded{}).refusal == Refusal::NoSaveRunning && idle.dirty());
    auto running = savingGate();
    const auto empty = running.saveFinished(std::unexpected(std::string()));
    TEST_CHECK(empty.refusal == Refusal::MissingSentence && empty.notice == Notice::None);
    TEST_CHECK(running.saving() && running.dirty());
  }

  EditGate dirtyGate() {
    EditGate gate;
    (void)gate.enable(true);
    (void)gate.setDirty(true);
    return gate;
  }

  void testCleanLeaveProceeds() {
    EditGate gate;
    (void)gate.enable(true);
    const auto left = gate.leave();
    TEST_CHECK(left.refusal == Refusal::None && left.proceed == Proceed::Leave && left.released == 1);
    TEST_CHECK(left.closeEditor && !left.openQuestion && !left.discardText && !left.startSave);
    TEST_CHECK(!gate.editing() && !gate.asking() && gate.pending() == 0);
    // Nothing is being edited, so there is nothing to keep.
    const auto idle = gate.leave();
    TEST_CHECK(idle.refusal == Refusal::None && idle.proceed == Proceed::Leave && !idle.closeEditor);
  }

  void testDirtyLeaveAsks() {
    auto gate = dirtyGate();
    const auto asked = gate.leave();
    TEST_CHECK(asked.refusal == Refusal::None && asked.openQuestion && asked.proceed == Proceed::None);
    TEST_CHECK(!asked.closeEditor && !asked.startSave);
    TEST_CHECK(gate.asking() && gate.editing() && gate.dirty() && gate.pending() == 1 && !gate.saveEnabled());
  }

  void testSaveIgnoredWhileAsking() {
    auto gate = dirtyGate();
    (void)gate.leave();
    const auto ignored = gate.save();
    TEST_CHECK(ignored.refusal == Refusal::QuestionOpen && !ignored.startSave && !gate.saving());
  }

  void testAnswerWithoutQuestionIsRefused() {
    auto gate = dirtyGate();
    TEST_CHECK(gate.answer(Answer::No).refusal == Refusal::NoQuestion && gate.dirty() && gate.editing());
    TEST_CHECK(gate.answer(Answer::Save).refusal == Refusal::NoQuestion && !gate.saving());
  }

  void testLeaveAnswerNo() {
    auto gate = dirtyGate();
    (void)gate.leave();
    const auto no = gate.answer(Answer::No);
    TEST_CHECK(no.refusal == Refusal::None && no.discardText && no.closeEditor && no.closeQuestion);
    TEST_CHECK(no.proceed == Proceed::Leave && no.released == 1 && !no.startSave);
    TEST_CHECK(!gate.editing() && !gate.dirty() && !gate.asking() && gate.pending() == 0);
  }

  void testLeaveSaveSucceeds() {
    auto gate = dirtyGate();
    (void)gate.leave();
    const auto save = gate.answer(Answer::Save);
    TEST_CHECK(save.refusal == Refusal::None && save.startSave && !save.closeQuestion);
    TEST_CHECK(save.proceed == Proceed::None && gate.saving() && gate.asking());
    // An open question is checked before a running save.
    TEST_CHECK(gate.save().refusal == Refusal::QuestionOpen);
    TEST_CHECK(gate.answer(Answer::No).refusal == Refusal::SaveRunning && gate.asking());
    const auto done = gate.saveFinished(SaveSucceeded{.changedSinceStart = false});
    TEST_CHECK(done.closeQuestion && done.closeEditor && !done.discardText && done.proceed == Proceed::Leave);
    TEST_CHECK(!gate.editing() && !gate.dirty() && !gate.saving() && !gate.asking() && gate.pending() == 0);
  }

  void testLeaveSaveFailsThenSucceeds() {
    auto gate = dirtyGate();
    (void)gate.leave();
    (void)gate.answer(Answer::Save);
    const auto failed = gate.saveFinished(std::unexpected(std::string("Permission denied.")));
    TEST_CHECK(failed.notice == Notice::SaveFailed && failed.sentence == "Permission denied.");
    TEST_CHECK(!failed.closeQuestion && !failed.openQuestion && failed.proceed == Proceed::None);
    TEST_CHECK(gate.asking() && gate.dirty() && gate.editing() && !gate.saving() && gate.pending() == 1);
    TEST_CHECK(gate.answer(Answer::Save).startSave);
    const auto done = gate.saveFinished(SaveSucceeded{.changedSinceStart = false});
    TEST_CHECK(done.closeQuestion && done.closeEditor && done.proceed == Proceed::Leave && done.released == 1);
    TEST_CHECK(!gate.editing() && !gate.asking());
  }

  void testQuestionStaysOpenWhileItsSaveRuns() {
    auto gate = dirtyGate();
    (void)gate.leave();
    (void)gate.answer(Answer::Save);
    TEST_CHECK(gate.answer(Answer::No).refusal == Refusal::SaveRunning && gate.asking() && gate.saving());
    const auto failed = gate.saveFinished(std::unexpected(std::string("Permission denied.")));
    TEST_CHECK(failed.notice == Notice::SaveFailed && !failed.openQuestion && !failed.closeQuestion);
    TEST_CHECK(gate.asking() && !gate.saving() && gate.pending() == 1);
    const auto no = gate.answer(Answer::No);
    TEST_CHECK(no.refusal == Refusal::None && no.discardText && no.closeQuestion && no.closeEditor);
    TEST_CHECK(no.proceed == Proceed::Leave && no.released == 1 && !gate.editing() && !gate.asking());
  }

  void testTwoLeavesOneAnswer() {
    auto gate = dirtyGate();
    TEST_CHECK(gate.leave().openQuestion);
    const auto second = gate.leave();
    TEST_CHECK(second.refusal == Refusal::None && !second.openQuestion && second.proceed == Proceed::None);
    TEST_CHECK(gate.pending() == 2 && gate.asking());
    const auto no = gate.answer(Answer::No);
    TEST_CHECK(no.proceed == Proceed::Leave && no.released == 2 && gate.pending() == 0);
  }

  void testSaveAnswerWhenNoLongerDirty() {
    auto gate = dirtyGate();
    (void)gate.leave();
    (void)gate.setDirty(false);
    const auto save = gate.answer(Answer::Save);
    TEST_CHECK(!save.startSave && !save.discardText && save.closeQuestion && save.closeEditor);
    TEST_CHECK(save.proceed == Proceed::Leave && !gate.editing() && !gate.saving());
  }

  void testLeaveDuringSaveThatSucceeds() {
    auto gate = savingGate();
    const auto wait = gate.leave();
    TEST_CHECK(wait.refusal == Refusal::None && !wait.openQuestion && wait.proceed == Proceed::None);
    TEST_CHECK(gate.saving() && !gate.asking() && gate.pending() == 1);
    const auto done = gate.saveFinished(SaveSucceeded{.changedSinceStart = false});
    TEST_CHECK(!done.openQuestion && !done.closeQuestion && done.closeEditor && !done.discardText);
    TEST_CHECK(done.proceed == Proceed::Leave && done.released == 1);
    TEST_CHECK(!gate.editing() && !gate.asking() && gate.pending() == 0);
  }

  void testLeaveDuringSaveThatFails() {
    auto gate = savingGate();
    (void)gate.leave();
    const auto failed = gate.saveFinished(std::unexpected(std::string("The file changed on disk.")));
    TEST_CHECK(failed.openQuestion && failed.notice == Notice::SaveFailed && failed.proceed == Proceed::None);
    TEST_CHECK(failed.sentence == "The file changed on disk." && !failed.closeEditor);
    TEST_CHECK(gate.asking() && gate.dirty() && gate.editing() && !gate.saving() && gate.pending() == 1);
    // Only a save that succeeded proceeds without asking, even when the text is clean again.
    auto undone = savingGate();
    (void)undone.leave();
    (void)undone.setDirty(false);
    const auto undoneFailed = undone.saveFinished(std::unexpected(std::string("Permission denied.")));
    TEST_CHECK(undoneFailed.openQuestion && undoneFailed.proceed == Proceed::None && undone.asking());
  }

  void testTypingDuringSave() {
    auto gate = savingGate();
    TEST_CHECK(gate.setDirty(true).refusal == Refusal::None && gate.saving());
    TEST_CHECK(gate.leave().proceed == Proceed::None);
    const auto done = gate.saveFinished(SaveSucceeded{.changedSinceStart = true});
    TEST_CHECK(done.openQuestion && done.notice == Notice::None && done.proceed == Proceed::None);
    TEST_CHECK(gate.asking() && gate.dirty() && !gate.saving() && gate.pending() == 1);
    TEST_CHECK(gate.answer(Answer::Save).startSave);
    const auto saved = gate.saveFinished(SaveSucceeded{.changedSinceStart = false});
    TEST_CHECK(saved.closeQuestion && saved.closeEditor && saved.proceed == Proceed::Leave && !gate.editing());
  }

  void testStop() {
    EditGate idle;
    TEST_CHECK(idle.stop().refusal == Refusal::NotEditing);
    TEST_CHECK(idle.enable(true).refusal == Refusal::None);
    const auto clean = idle.stop();
    TEST_CHECK(clean.closeEditor && clean.proceed == Proceed::Stay && clean.released == 1 && !idle.editing());

    auto gate = dirtyGate();
    const auto asked = gate.stop();
    TEST_CHECK(asked.openQuestion && asked.proceed == Proceed::None && gate.asking());
    const auto no = gate.answer(Answer::No);
    TEST_CHECK(no.discardText && no.closeEditor && no.closeQuestion && no.proceed == Proceed::Stay);
    TEST_CHECK(!gate.editing() && !gate.dirty() && !gate.asking());
  }

  void testStopThenLeaveProceedsToLeave() {
    auto gate = dirtyGate();
    (void)gate.stop();
    TEST_CHECK(gate.leave().refusal == Refusal::None && gate.pending() == 2);
    const auto no = gate.answer(Answer::No);
    TEST_CHECK(no.proceed == Proceed::Leave && no.released == 2);
    auto leaveFirst = dirtyGate();
    (void)leaveFirst.leave();
    (void)leaveFirst.stop();
    TEST_CHECK(leaveFirst.answer(Answer::No).proceed == Proceed::Leave);
    // The next session starts without the earlier leave.
    (void)leaveFirst.enable(true);
    TEST_CHECK(leaveFirst.stop().proceed == Proceed::Stay);
    // A stop while saving waits like a leave.
    auto saving = savingGate();
    TEST_CHECK(saving.stop().proceed == Proceed::None && saving.pending() == 1 && !saving.asking());
    TEST_CHECK(saving.saveFinished(SaveSucceeded{}).proceed == Proceed::Stay);
  }

  void testChangedOnDisk() {
    EditGate idle;
    TEST_CHECK(idle.changedOnDisk().refusal == Refusal::NotEditing);
    auto gate = savingGate();
    (void)gate.leave();
    const auto notice = gate.changedOnDisk();
    TEST_CHECK(notice.refusal == Refusal::None && notice.notice == Notice::ChangedOnDisk);
    TEST_CHECK(!notice.discardText && !notice.closeEditor && !notice.startSave && notice.proceed == Proceed::None);
    TEST_CHECK(gate.editing() && gate.dirty() && gate.saving() && gate.pending() == 1);
  }

  void testCleanFlagAfterEditingEnds() {
    auto gate = dirtyGate();
    (void)gate.leave();
    (void)gate.answer(Answer::No);
    // Reverting the discarded text clears the modified flag; that agrees with the gate.
    TEST_CHECK(gate.setDirty(false).refusal == Refusal::None && !gate.dirty() && !gate.editing());
    TEST_CHECK(gate.setDirty(true).refusal == Refusal::NotEditing && !gate.dirty());
    TEST_CHECK(gate.enable(true).refusal == Refusal::None && !gate.dirty() && gate.pending() == 0);
  }

  struct Snapshot {
    bool editing = false;
    bool dirty = false;
    bool saving = false;
    bool asking = false;
    bool saveEnabled = false;
    std::size_t pending = 0;
    bool operator==(const Snapshot&) const = default;
  };

  Snapshot snapshot(const EditGate& gate) {
    return {gate.editing(), gate.dirty(), gate.saving(), gate.asking(), gate.saveEnabled(), gate.pending()};
  }

  bool hasEffects(const GateResult& result) {
    return result.startSave
        || result.openQuestion
        || result.closeQuestion
        || result.discardText
        || result.closeEditor
        || result.notice != Notice::None
        || !result.sentence.empty()
        || result.proceed != Proceed::None
        || result.released != 0;
  }

  // xorshift64 with a fixed seed, so every run walks the same path.
  struct Generator {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    std::uint64_t next() {
      state ^= state << 13U;
      state ^= state >> 7U;
      state ^= state << 17U;
      return state;
    }
  };

  enum class Event : std::uint8_t {
    Enable,
    EnableReadOnly,
    Type,
    Undo,
    Save,
    SavedClean,
    SavedTyped,
    SaveFailed,
    SaveFailedSilently,
    Leave,
    Stop,
    AnswerSave,
    AnswerNo,
    ChangedOnDisk,
    Count,
  };

  GateResult apply(EditGate& gate, Event event) {
    switch (event) {
    case Event::Enable:
      return gate.enable(true);
    case Event::EnableReadOnly:
      return gate.enable(false);
    case Event::Type:
      return gate.setDirty(true);
    case Event::Undo:
      return gate.setDirty(false);
    case Event::Save:
      return gate.save();
    case Event::SavedClean:
      return gate.saveFinished(SaveSucceeded{.changedSinceStart = false});
    case Event::SavedTyped:
      return gate.saveFinished(SaveSucceeded{.changedSinceStart = true});
    case Event::SaveFailed:
      return gate.saveFinished(std::unexpected(std::string("No space left on the device.")));
    case Event::SaveFailedSilently:
      return gate.saveFinished(std::unexpected(std::string()));
    case Event::Leave:
      return gate.leave();
    case Event::Stop:
      return gate.stop();
    case Event::AnswerSave:
      return gate.answer(Answer::Save);
    case Event::AnswerNo:
      return gate.answer(Answer::No);
    case Event::ChangedOnDisk:
    case Event::Count:
      break;
    }
    return gate.changedOnDisk();
  }

  struct WalkCounts {
    std::size_t refused = 0;
    std::size_t saves = 0;
    std::size_t questions = 0;
    std::size_t proceeds = 0;
    std::size_t discards = 0;
  };

  // leavePending: an accepted leave() is waiting for a proceed.
  void
  checkStep(const EditGate& gate, const Snapshot& before, Event event, const GateResult& result, bool leavePending) {
    const auto after = snapshot(gate);
    const bool request = event == Event::Leave || event == Event::Stop;
    if (result.refusal != Refusal::None) {
      TEST_CHECK(!hasEffects(result) && after == before);
    }
    TEST_CHECK(after.saveEnabled == (after.editing && after.dirty && !after.saving && !after.asking));
    TEST_CHECK(!after.asking || (after.editing && after.pending > 0));
    TEST_CHECK(after.pending == 0 || after.asking || after.saving);
    TEST_CHECK(after.editing || (!after.dirty && !after.saving && !after.asking && after.pending == 0));
    TEST_CHECK(!result.startSave || (before.dirty && !before.saving && after.saving));
    TEST_CHECK(!result.discardText || (before.dirty && !after.editing));
    TEST_CHECK(result.openQuestion == (!before.asking && after.asking));
    TEST_CHECK(result.closeQuestion == (before.asking && !after.asking));
    TEST_CHECK(result.closeEditor == (before.editing && !after.editing));
    TEST_CHECK((result.proceed != Proceed::None) == (result.released > 0));
    if (result.proceed != Proceed::None) {
      TEST_CHECK(result.released == before.pending + (request ? 1U : 0U) && after.pending == 0);
      const bool toLeave = leavePending || event == Event::Leave;
      TEST_CHECK(result.proceed == (toLeave ? Proceed::Leave : Proceed::Stay));
    }
    TEST_CHECK((result.notice == Notice::SaveFailed) == !result.sentence.empty());
  }

  void testRandomWalk() {
    constexpr std::size_t kSteps = 100'000;
    Generator generator;
    EditGate gate;
    bool saveStarted = false;
    bool leavePending = false;
    WalkCounts counts;
    const auto start = std::clock();
    for (std::size_t step = 0; step < kSteps; ++step) {
      const auto event = static_cast<Event>(generator.next() % static_cast<std::uint64_t>(Event::Count));
      const auto before = snapshot(gate);
      auto probe = gate;
      const bool saveAccepted = probe.save().startSave;
      TEST_CHECK(saveAccepted == before.saveEnabled);
      const auto result = apply(gate, event);
      checkStep(gate, before, event, result, leavePending);
      if (result.proceed != Proceed::None) {
        leavePending = false;
      } else if (event == Event::Leave && before.editing && result.refusal == Refusal::None) {
        leavePending = true;
      }
      if (result.startSave) {
        saveStarted = true;
        ++counts.saves;
      }
      if (gate.saving() != before.saving && !gate.saving()) {
        saveStarted = false;
      }
      TEST_CHECK(gate.saving() == saveStarted);
      counts.refused += result.refusal != Refusal::None ? 1U : 0U;
      counts.questions += result.openQuestion ? 1U : 0U;
      counts.proceeds += result.proceed != Proceed::None ? 1U : 0U;
      counts.discards += result.discardText ? 1U : 0U;
    }
    const auto seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    std::println(
        "random walk: {} steps, {} refused, {} saves, {} questions, {} proceeds, {} discards, {:.3f} s cpu", kSteps,
        counts.refused, counts.saves, counts.questions, counts.proceeds, counts.discards, seconds
    );
    TEST_CHECK(counts.saves > 500 && counts.questions > 500 && counts.proceeds > 1000 && counts.discards > 100);
  }
} // namespace

int main() {
  testEnableNeedsEditableFile();
  testEnableWhileEditingIsRefused();
  testSaveNeedsChanges();
  testSaveFinished();
  testSaveFinishedRefusals();
  testCleanLeaveProceeds();
  testDirtyLeaveAsks();
  testSaveIgnoredWhileAsking();
  testAnswerWithoutQuestionIsRefused();
  testLeaveAnswerNo();
  testLeaveSaveSucceeds();
  testLeaveSaveFailsThenSucceeds();
  testQuestionStaysOpenWhileItsSaveRuns();
  testTwoLeavesOneAnswer();
  testSaveAnswerWhenNoLongerDirty();
  testLeaveDuringSaveThatSucceeds();
  testLeaveDuringSaveThatFails();
  testTypingDuringSave();
  testStop();
  testStopThenLeaveProceedsToLeave();
  testChangedOnDisk();
  testCleanFlagAfterEditingEnds();
  testRandomWalk();
  std::println("editing_edit_gate_test: ok");
  return 0;
}
