#include "app/page/workbench_controller.h"
#include "app/page/workbench_preview.h"
#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "fs/location_within.h"
#include "media/media_player.h"
#include "media/media_probe.h"

#include <cstddef>

// What a drive's removal asks of the Workbench page: whether it is still using the drive, and to let go of it.

bool WorkbenchController::inDrive(const luciole::Location& root) const {
  if (m_editor && m_editor->savingIn(root)) {
    return true;
  }
  if (!m_session) {
    return false;
  }
  const auto& lookahead = m_session->lookahead();
  for (std::size_t column = 0; column < lookahead.count(); ++column) {
    if (luciole::locationWithin(lookahead.location(column), root)) {
      return true;
    }
  }
  return m_columnPreview && m_columnPreview->reading(root);
}

bool WorkbenchController::unsavedIn(const luciole::Location& root) { return m_editor && m_editor->unsavedIn(root); }

void WorkbenchController::releaseLocation(const luciole::Location& root) {
  if (!m_session) {
    return;
  }
  m_session->model().releaseLocation(root);
  m_session->lookahead().releaseLocation(root);
  if (m_columnPreview) {
    m_columnPreview->releaseLocation(root);
  }
}

bool WorkbenchController::readingDrive(const luciole::Location& root) const {
  return (m_services != nullptr && m_services->measureService().reading(root))
      || (m_session && m_session->lookahead().reading(root))
      || (m_editor && m_editor->savingIn(root))
      || luciole::MediaPlayer::stopsPending()
      || luciole::MediaProbe::readsPending()
      || (m_columnPreview && m_columnPreview->reading(root));
}
