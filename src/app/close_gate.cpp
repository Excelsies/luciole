#include "app/close_gate.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

void luciole::CloseGate::request(
    std::vector<Page> pages, std::function<void()> action, std::function<void(Abandon)> abandoned
) {
  if (!action || std::ranges::any_of(pages, [](const Page& page) { return !page.unsaved || !page.ask; })) {
    throw std::invalid_argument("A close request needs its action and every page's callbacks.");
  }
  if (m_active) {
    if (abandoned) {
      abandoned(Abandon::Busy);
    }
    return;
  }
  m_pages = std::move(pages);
  m_action = std::move(action);
  m_abandoned = std::move(abandoned);
  m_next = 0;
  m_asked = false;
  m_active = true;
  advance();
}

void luciole::CloseGate::advance() {
  // Each pass moves to the next page, returns to wait or ends the request, and each page is asked once, so the loop
  // runs at most twice per page.
  while (m_next < m_pages.size()) {
    if (!m_pages[m_next].unsaved()) {
      ++m_next;
      m_asked = false;
      continue;
    }
    if (m_asked) {
      abandon(Abandon::Kept);
      return;
    }
    m_asked = true;
    m_asking = true;
    m_answered = false;
    const std::weak_ptr<int> lifetime = m_lifetime;
    const bool now = m_pages[m_next].ask([this, lifetime, ask = ++m_ask] {
      if (!lifetime.expired()) {
        resume(ask);
      }
    });
    m_asking = false;
    if (now) {
      ++m_next;
      m_asked = false;
      continue;
    }
    if (!m_answered) {
      m_waiting = true;
      return;
    }
  }
  auto action = std::move(m_action);
  end();
  // The action may start another request or end the gate's life.
  const std::weak_ptr<int> lifetime = m_lifetime;
  const bool outer = std::exchange(m_acting, true);
  try {
    action();
  } catch (...) {
    if (!lifetime.expired()) {
      m_acting = outer;
    }
    throw;
  }
  if (!lifetime.expired()) {
    m_acting = outer;
  }
}

void luciole::CloseGate::resume(std::uint64_t ask) {
  if (!m_active || ask != m_ask) {
    return;
  }
  if (m_asking) {
    m_answered = true;
    return;
  }
  if (!m_waiting) {
    return;
  }
  m_waiting = false;
  advance();
}

void luciole::CloseGate::end() {
  m_active = false;
  m_waiting = false;
  m_pages.clear();
  m_action = {};
  m_abandoned = {};
}

void luciole::CloseGate::abandon(Abandon reason) {
  auto abandoned = std::move(m_abandoned);
  end();
  if (abandoned) {
    abandoned(reason);
  }
}
