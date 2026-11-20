#pragma once

#include "model/directory_options.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

class Segmented;

namespace luciole {

  // A change of view mode that waits while the question about unsaved text is open. Only the last request applies,
  // once the question is answered No or the save succeeded. The toolbar's view segment shows the view that is current.
  class HeldViewChange {
  public:
    struct Hooks {
      std::function<ViewMode()> current;
      // The toolbar segment of a view mode.
      std::function<std::size_t(ViewMode)> segmentOf;
      // The page's leave: true when the page may go now, otherwise `resume` follows the answer.
      std::function<bool(std::function<void()> resume)> leave;
      std::function<bool()> unsaved;
      // Takes `mode` as the current view.
      std::function<void(ViewMode)> apply;
    };

    explicit HeldViewChange(Hooks hooks);
    // The resume captures this object.
    HeldViewChange(const HeldViewChange&) = delete;
    HeldViewChange& operator=(const HeldViewChange&) = delete;

    // The segment control this change keeps in step; null until the toolbar is built.
    void setSegment(Segmented* segment) noexcept { m_segment = segment; }
    // The segment's change report: `dispatch(index)` runs unless the change was made by show().
    void changed(std::size_t index, const std::function<void(std::size_t)>& dispatch);
    void request(ViewMode mode);
    // Sets the segment to `mode`'s without reporting a change.
    void show(ViewMode mode);

  private:
    void applyHeld();
    // Asks the page to leave; its answer resumes the held change.
    bool leaveForHeld();

    Hooks m_hooks;
    Segmented* m_segment = nullptr;
    bool m_showing = false;
    std::optional<ViewMode> m_held;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };

} // namespace luciole
