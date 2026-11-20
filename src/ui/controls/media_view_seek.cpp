#include "ui/controls/label.h"
#include "ui/controls/media_view.h"

#include <utility>

// The note that a seek is taking time. A seek that settles within kSeekNoticeDelay shows nothing; one in flight for
// longer puts "Seeking..." in the message label beside the time, until it settles, fails, or the player stops. Each
// new seek starts the wait again.
void MediaView::watchSeek() {
  m_seekSince = std::chrono::steady_clock::now();
  if (m_seekShown) {
    m_seekShown = false;
    refreshControls();
  }
  if (!m_seekWatch.active()) {
    m_seekWatch.startRepeating(kSeekWatchInterval, [this] { seekTick(); });
  }
}

void MediaView::seekTick() {
  const bool inFlight = m_player && m_player->seeking() && m_player->state() != luciole::MediaState::Error;
  const bool late = inFlight && std::chrono::steady_clock::now() - m_seekSince >= kSeekNoticeDelay;
  if (late != m_seekShown) {
    m_seekShown = late;
    refreshControls();
  }
  if (!inFlight) {
    m_seekWatch.stop();
  }
}

void MediaView::clearSeek() {
  m_seekWatch.stop();
  if (std::exchange(m_seekShown, false)) {
    refreshControls();
  }
}
