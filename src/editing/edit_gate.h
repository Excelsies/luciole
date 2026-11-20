#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace luciole::editing {

  // Why an event changed nothing. None means the event was accepted.
  enum class Refusal : std::uint8_t {
    None,
    NotEditable,
    AlreadyEditing,
    NotEditing,
    NothingToSave,
    SaveRunning,
    QuestionOpen,
    NoQuestion,
    NoSaveRunning,
    MissingSentence,
  };

  // A sentence the controller shows.
  enum class Notice : std::uint8_t {
    None,
    SaveFailed,
    // The file changed on disk while it is being edited.
    ChangedOnDisk,
  };

  // What the pending requests may now do.
  enum class Proceed : std::uint8_t {
    None,
    // Show something else or go away.
    Leave,
    // Close the editor and stay on this file.
    Stay,
  };

  // The buttons of the "Save changes to <name>?" question.
  enum class Answer : std::uint8_t {
    Save,
    No,
  };

  // A background save that wrote the file.
  struct SaveSucceeded {
    bool changedSinceStart = false;
  };

  // What the controller must do after reporting one event. A refused event has no effects.
  struct [[nodiscard]] GateResult {
    Refusal refusal = Refusal::None;
    bool startSave = false;
    bool openQuestion = false;
    bool closeQuestion = false;
    // Drop the unsaved changes.
    bool discardText = false;
    bool closeEditor = false;
    Notice notice = Notice::None;
    // The failure sentence when notice is SaveFailed.
    std::string sentence;
    Proceed proceed = Proceed::None;
    // How many requests the proceed releases.
    std::size_t released = 0;
  };

  // The rules of editing a file inside a preview. Holds no text and does no I/O.
  class EditGate {
  public:
    GateResult enable(bool editable);
    // Reports the text model's modified flag. Changes are refused while not editing; clean is accepted.
    GateResult setDirty(bool dirty);
    GateResult save();
    // Reports the end of the background save, or its failure sentence.
    GateResult saveFinished(std::expected<SaveSucceeded, std::string> outcome);
    // The preview is about to show something else or go away. Proceeds at once when not editing.
    GateResult leave();
    // The editor's Stop editing button or Escape. Proceeds with Stay unless a leave is pending.
    GateResult stop();
    // Refused with SaveRunning while the question's save runs; the question stays open until saveFinished.
    // A failed save keeps it open with the sentence for another answer, so the dialog must not close meanwhile.
    GateResult answer(Answer choice);
    // Asks for a notice; keeps the text and the editor.
    GateResult changedOnDisk();

    bool editing() const { return m_editing; }
    bool dirty() const { return m_dirty; }
    bool saving() const { return m_saving; }
    bool asking() const { return m_asking; }
    bool saveEnabled() const { return m_editing && m_dirty && !m_saving && !m_asking; }
    // Requests waiting for a proceed.
    std::size_t pending() const { return m_pending; }

  private:
    GateResult request(bool leaving);
    GateResult proceed(bool discard);

    bool m_editing = false;
    bool m_dirty = false;
    bool m_saving = false;
    bool m_asking = false;
    std::size_t m_pending = 0;
    // A pending request is a leave.
    bool m_leaving = false;
  };

} // namespace luciole::editing
