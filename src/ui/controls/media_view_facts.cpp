#include "ui/controls/media_view.h"

// Reading what the container says of the file. The first read has the probe's usual time limit. When it ends because
// the time ran out, which says nothing of the file, one more read follows with a longer limit, as the first one
// loaded GStreamer's plugins; the message says it is still reading meanwhile. When that times out too, the details
// stay unknown and the message says so. Never more than these two reads for a path, and none for a file that is not
// media, which ends at once. Choosing the file again after a failure reads again.
void MediaView::readFacts() {
  if (m_path.empty() || m_factsRead || !m_shown || m_probe.busy()) {
    return;
  }
  const auto limit = m_factsRetried ? m_retryLimit : m_firstLimit;
  m_probe.start(m_path, [this](std::optional<luciole::MediaFacts> facts) { factsArrived(facts); }, limit);
}

// Keeps a playing time the player found when the container states none.
void MediaView::factsArrived(std::optional<luciole::MediaFacts> facts) {
  if (!facts && m_probe.timedOut()) {
    if (!m_factsRetried) {
      m_factsRetried = true;
      readFacts();
    } else {
      m_factsRead = true;
      m_factsFailed = true;
    }
    refreshControls();
    return;
  }
  m_factsRead = true;
  if (facts) {
    facts->seconds = facts->seconds > 0 ? facts->seconds : m_facts.seconds;
    m_facts = *facts;
    refreshControls();
    if (onFactsChanged) {
      onFactsChanged();
    }
  }
}
