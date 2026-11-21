#include "app/page/preview_edit_controller.h"
#include "core/deferred_call.h"

#include <utility>

// Leaving a file that is edited, and what closing the question about it without a choice means.
namespace luciole {
  void PreviewEditController::noteRequest(bool dismissCancels) {
    if (!m_gate.asking() && m_gate.pending() == 0) {
      m_cancelLeaves = false;
      m_otherRequests = false;
    }
    (dismissCancels ? m_cancelLeaves : m_otherRequests) = true;
  }

  bool PreviewEditController::dismissCancelsLeave() {
    const bool chosen = std::exchange(m_noChosen, false);
    if (chosen || !m_cancelLeaves || m_otherRequests) {
      return false;
    }
    if (m_gate.cancel().refusal == editing::Refusal::SaveRunning) {
      m_question = QuestionState::Wanted; // closed while its save runs: no answer, it comes back
      return true;
    }
    m_leaving = false;
    m_cancelLeaves = false;
    // The changes are still unsaved, so each waiting request gives up.
    for (auto& resume : std::exchange(m_resumes, {})) {
      DeferredCall::callLater(std::move(resume));
    }
    refresh();
    return true;
  }

  bool PreviewEditController::leave(std::function<void()> resume, bool dismissCancels) {
    if (!m_session) {
      return true;
    }
    requestLeave(dismissCancels);
    if (!m_session) {
      return true; // nothing was unsaved: the editor closed at once
    }
    m_resumes.push_back(std::move(resume));
    return false;
  }

  void PreviewEditController::requestLeave(bool dismissCancels) {
    if (!m_session) {
      return;
    }
    noteRequest(dismissCancels);
    if (!m_leaving) {
      settle();
      m_leaving = true;
      apply(m_gate.leave());
    }
  }
} // namespace luciole
