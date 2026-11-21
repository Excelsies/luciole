#include "ui/controls/media_view.h"

#include <algorithm>

// A video that sits paused or ended for kIdleRelease gives its pipeline back. The frame on the stage, the time and
// the bar stay as they were; Play, or a seek, opens the file again at the same place. Sound files keep their player:
// a paused one holds little.

bool MediaView::idleVideo() const {
  if (!m_player || m_dragging || m_player->seeking() || !m_player->hasVideo() || !m_player->reopensInPlace()) {
    return false;
  }
  const auto state = m_player->state();
  return state == luciole::MediaState::Ended || (state == luciole::MediaState::Paused && !m_playRequested);
}

// Starts the wait when the video went idle, keeps it running while it stays so, and stops it otherwise. A seek in
// flight is not idle, so asking after one drops the old deadline, also when the seek ends where the video was.
void MediaView::watchIdle() {
  if (!idleVideo()) {
    m_idle.stop();
  } else if (!m_idle.active()) {
    m_idle.start(m_idleRelease, [this] { park(); });
  }
}

void MediaView::park() {
  if (!idleVideo()) {
    return;
  }
  const bool ended = m_player->state() == luciole::MediaState::Ended;
  m_parked = Parked{.at = ended ? m_player->duration() : m_player->position(), .duration = m_player->duration(),
                    .ended = ended};
  m_player.reset();
  m_clock.stop();
  clearSeek();
  m_playRequested = false;
  refreshControls();
}

double MediaView::knownDuration() const {
  if (m_player && m_player->duration() > 0) {
    return m_player->duration();
  }
  return m_parked ? m_parked->duration : 0;
}

// The player, opening a parked file again if need be, paused at the place it was parked.
bool MediaView::wake() {
  if (m_player) {
    return true;
  }
  if (!m_parked || !openPlayer()) {
    return false;
  }
  m_player->seek(m_parked->at);
  return true;
}
