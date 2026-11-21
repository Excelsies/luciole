#include "app/page/preview_edit_controller.h"
#include "core/deferred_call.h"

#include <utility>

namespace luciole {
  bool PreviewEditController::dismissCancelsLeave() {
    if (std::exchange(m_noChosen, false) || !m_dismissCancels) {
      return false;
    }
    if (m_gate.cancel().refusal == editing::Refusal::SaveRunning) {
      m_question = QuestionState::Wanted; // closed while its save runs: no answer, it comes back
      return true;
    }
    m_leaving = false;
    // The changes are still unsaved, so each waiting request gives up.
    for (auto& resume : std::exchange(m_resumes, {})) {
      DeferredCall::callLater(std::move(resume));
    }
    refresh();
    return true;
  }
} // namespace luciole
