#include "editing/edit_gate.h"

#include <utility>

namespace luciole::editing {

  namespace {
    GateResult refused(Refusal why) {
      GateResult result;
      result.refusal = why;
      return result;
    }
  } // namespace

  GateResult EditGate::enable(bool editable) {
    if (m_editing) {
      return refused(Refusal::AlreadyEditing);
    }
    if (!editable) {
      return refused(Refusal::NotEditable);
    }
    m_editing = true;
    m_dirty = false;
    return {};
  }

  GateResult EditGate::setDirty(bool dirty) {
    if (!m_editing && dirty) {
      return refused(Refusal::NotEditing);
    }
    m_dirty = dirty;
    return {};
  }

  GateResult EditGate::save() {
    if (!m_editing) {
      return refused(Refusal::NotEditing);
    }
    if (m_asking) {
      return refused(Refusal::QuestionOpen);
    }
    if (m_saving) {
      return refused(Refusal::SaveRunning);
    }
    if (!m_dirty) {
      return refused(Refusal::NothingToSave);
    }
    m_saving = true;
    GateResult result;
    result.startSave = true;
    return result;
  }

  GateResult EditGate::saveFinished(std::expected<SaveSucceeded, std::string> outcome) {
    if (!m_saving) {
      return refused(Refusal::NoSaveRunning);
    }
    if (!outcome.has_value() && outcome.error().empty()) {
      return refused(Refusal::MissingSentence);
    }
    m_saving = false;
    GateResult result;
    if (outcome.has_value()) {
      m_dirty = outcome->changedSinceStart;
    } else {
      result.notice = Notice::SaveFailed;
      result.sentence = std::move(outcome.error());
    }
    if (m_pending == 0) {
      return result;
    }
    if (outcome.has_value() && !m_dirty) {
      return proceed(false);
    }
    if (!m_asking) {
      m_asking = true;
      result.openQuestion = true;
    }
    return result;
  }

  GateResult EditGate::leave() {
    if (!m_editing) {
      GateResult result;
      result.proceed = Proceed::Leave;
      result.released = 1;
      return result;
    }
    return request(true);
  }

  GateResult EditGate::stop() {
    if (!m_editing) {
      return refused(Refusal::NotEditing);
    }
    return request(false);
  }

  GateResult EditGate::request(bool leaving) {
    ++m_pending;
    m_leaving = m_leaving || leaving;
    if (m_asking || m_saving) {
      return {};
    }
    if (!m_dirty) {
      return proceed(false);
    }
    m_asking = true;
    GateResult result;
    result.openQuestion = true;
    return result;
  }

  GateResult EditGate::answer(Answer choice) {
    if (!m_asking) {
      return refused(Refusal::NoQuestion);
    }
    if (m_saving) {
      return refused(Refusal::SaveRunning);
    }
    if (choice == Answer::No) {
      return proceed(true);
    }
    if (!m_dirty) {
      return proceed(false);
    }
    m_saving = true;
    GateResult result;
    result.startSave = true;
    return result;
  }

  GateResult EditGate::changedOnDisk() {
    if (!m_editing) {
      return refused(Refusal::NotEditing);
    }
    GateResult result;
    result.notice = Notice::ChangedOnDisk;
    return result;
  }

  GateResult EditGate::proceed(bool discard) {
    GateResult result;
    result.discardText = discard && m_dirty;
    result.closeQuestion = m_asking;
    result.closeEditor = true;
    result.proceed = m_leaving ? Proceed::Leave : Proceed::Stay;
    result.released = m_pending;
    m_editing = false;
    m_dirty = false;
    m_asking = false;
    m_pending = 0;
    m_leaving = false;
    return result;
  }

} // namespace luciole::editing
