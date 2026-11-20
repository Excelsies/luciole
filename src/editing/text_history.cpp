#include "editing/text_history.h"

#include <algorithm>
#include <utility>

namespace {
  std::size_t storedBytes(const luciole::editing::HistoryStep& step) {
    return step.removed.size() + step.inserted.size();
  }
} // namespace

luciole::editing::TextPosition luciole::editing::endOfText(TextPosition start, std::string_view text) {
  const auto lastLf = text.rfind('\n');
  if (lastLf == std::string_view::npos) {
    return {start.line, start.byte + text.size()};
  }
  const auto breaks = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  return {start.line + breaks, text.size() - lastLf - 1};
}

luciole::editing::TextHistory::TextHistory() : m_stateIds{0}, m_savedStateId(0), m_revisionStates{0} {}

void luciole::editing::TextHistory::record(HistoryStep step, StepKind kind, std::uint64_t nowMs) {
  if (joins(step, kind, nowMs)) {
    join(step, kind);
    m_stateIds[m_current] = m_nextStateId++;
  } else {
    // Bounded by the steps after the current position.
    while (m_steps.size() > m_current) {
      m_storedBytes -= storedBytes(m_steps.back());
      m_steps.pop_back();
      m_stateIds.pop_back();
    }
    m_storedBytes += storedBytes(step);
    m_steps.push_back(std::move(step));
    m_stateIds.push_back(m_nextStateId++);
    ++m_current;
  }
  m_openGroup = kind == StepKind::Other ? std::nullopt : std::optional(kind);
  m_groupTimeMs = nowMs;
  dropOldest();
  advanceRevision();
}

bool luciole::editing::TextHistory::joins(const HistoryStep& step, StepKind kind, std::uint64_t nowMs) const {
  if (m_openGroup != kind || m_current == 0 || m_current != m_steps.size()) {
    return false;
  }
  if (nowMs < m_groupTimeMs || nowMs - m_groupTimeMs > kTypingGroupMs) {
    return false;
  }
  const auto& last = m_steps.back();
  switch (kind) {
  case StepKind::Typing:
    return step.removed.empty() && step.start == endOfText(last.start, last.inserted);
  case StepKind::EraseBackward:
    return step.inserted.empty() && endOfText(step.start, step.removed) == last.start;
  case StepKind::EraseForward:
    return step.inserted.empty() && step.start == last.start;
  case StepKind::Other:
    break;
  }
  return false;
}

// A joined backward erase prepends, copying the group's removed text once. A group only removes document text, so
// that copy is at most the 1 MiB text limit.
void luciole::editing::TextHistory::join(const HistoryStep& step, StepKind kind) {
  auto& last = m_steps.back();
  m_storedBytes += storedBytes(step);
  if (kind == StepKind::Typing) {
    last.inserted += step.inserted;
  } else if (kind == StepKind::EraseBackward) {
    last.removed.insert(0, step.removed);
    last.start = step.start;
  } else {
    last.removed += step.removed;
  }
  last.after = step.after;
}

const luciole::editing::HistoryStep* luciole::editing::TextHistory::undoStep() {
  if (m_current == 0) {
    return nullptr;
  }
  closeGroup();
  --m_current;
  advanceRevision();
  return &m_steps[m_current];
}

const luciole::editing::HistoryStep* luciole::editing::TextHistory::redoStep() {
  if (m_current == m_steps.size()) {
    return nullptr;
  }
  closeGroup();
  ++m_current;
  advanceRevision();
  return &m_steps[m_current - 1];
}

void luciole::editing::TextHistory::closeGroup() { m_openGroup.reset(); }

bool luciole::editing::TextHistory::markSaved(std::uint64_t revision) {
  const auto logged = revision >= m_firstLoggedRevision && revision <= m_revision;
  const auto state = logged ? std::optional(m_revisionStates[revision - m_firstLoggedRevision]) : std::nullopt;
  if (!state || !reachable(*state)) {
    m_savedStateId.reset();
    return false;
  }
  m_savedStateId = state;
  if (*state == m_stateIds[m_current]) {
    closeGroup();
  }
  return true;
}

bool luciole::editing::TextHistory::modified() const { return m_savedStateId != m_stateIds[m_current]; }

void luciole::editing::TextHistory::dropOldest() {
  // Each pass drops one step and keeps the newest, which alone never exceeds kHistoryBytes.
  while (m_steps.size() > 1 && (m_steps.size() > kHistorySteps || m_storedBytes > kHistoryBytes)) {
    m_storedBytes -= storedBytes(m_steps.front());
    m_steps.pop_front();
    m_stateIds.pop_front();
    --m_current;
  }
}

void luciole::editing::TextHistory::advanceRevision() {
  ++m_revision;
  m_revisionStates.push_back(m_stateIds[m_current]);
  if (m_revisionStates.size() > kRevisionMemory) {
    m_revisionStates.pop_front();
    ++m_firstLoggedRevision;
  }
}

bool luciole::editing::TextHistory::reachable(std::uint64_t stateId) const {
  return std::ranges::contains(m_stateIds, stateId);
}
