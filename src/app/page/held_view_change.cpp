#include "app/page/held_view_change.h"

#include "ui/controls/segmented.h"

#include <stdexcept>
#include <utility>

namespace luciole {

  HeldViewChange::HeldViewChange(Hooks hooks) : m_hooks(std::move(hooks)) {
    if (!m_hooks.current || !m_hooks.segmentOf || !m_hooks.leave || !m_hooks.unsaved || !m_hooks.apply) {
      throw std::invalid_argument("A held view change needs every hook.");
    }
  }

  void HeldViewChange::changed(std::size_t index, const std::function<void(std::size_t)>& dispatch) {
    if (!m_showing) {
      dispatch(index);
    }
  }

  void HeldViewChange::show(ViewMode mode) {
    if (m_segment == nullptr) {
      return;
    }
    // Segmented reports every index change, also one made from here.
    struct Showing {
      bool& flag;
      bool before;
      explicit Showing(bool& value) : flag(value), before(value) { flag = true; }
      ~Showing() { flag = before; }
      Showing(const Showing&) = delete;
      Showing& operator=(const Showing&) = delete;
    };
    const Showing showing(m_showing);
    m_segment->setSelectedIndex(m_hooks.segmentOf(mode));
  }

  bool HeldViewChange::leaveForHeld() {
    return m_hooks.leave([this, lifetime = std::weak_ptr<int>(m_lifetime)] {
      if (!lifetime.expired()) {
        applyHeld();
      }
    });
  }

  // While a mode is held the last request replaces it. Unsaved text is asked about while the editor is still shown;
  // the change goes on once the question is answered. Each request with unsaved text asks the page to leave again, so
  // a resume the page dropped is replaced; the first resume to run clears the held mode and the others find none.
  void HeldViewChange::request(ViewMode mode) {
    const bool held = m_held.has_value();
    if (!held && mode == m_hooks.current()) {
      return;
    }
    m_held = mode;
    if (held && !m_hooks.unsaved()) {
      applyHeld();
      return;
    }
    if (leaveForHeld()) {
      applyHeld();
      return;
    }
    show(m_hooks.current());
  }

  void HeldViewChange::applyHeld() {
    const auto held = std::exchange(m_held, std::nullopt);
    if (!held) {
      return;
    }
    if (*held != m_hooks.current()) {
      m_hooks.apply(*held);
    }
    show(m_hooks.current());
  }

} // namespace luciole
