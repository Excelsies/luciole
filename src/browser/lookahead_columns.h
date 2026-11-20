#pragma once

#include "browser/browser_controller.h"
#include "browser/workbench_state.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace luciole {
  // What the columns ahead follow. Column one lists the target when it is a folder: the page state's target, or with
  // none the a of workbench::columnChain, which is then the remembered or heaviest folder. Each later column lists
  // the entry of the column before it that its pin names, else its pick, when that entry lies directly in the folder
  // the column before lists. The picks are the chain rule's: the b and c of workbench::columnChain for the measured
  // tree of the page's folder, with the target as pointer or cursor and the pins as steerB and steerC. A file stops
  // the chain and is previewed in the next slot.
  struct LookaheadRequest {
    std::optional<WorkbenchEntry> target = std::nullopt;
    std::array<std::optional<WorkbenchEntry>, WorkbenchState::kPinnedColumns> pins{};
    std::array<std::optional<WorkbenchEntry>, WorkbenchState::kPinnedColumns> picks{};
  };

  // The file the columns ahead preview, and the slot it starts in (0 is column one).
  struct LookaheadPreview {
    Location file;
    std::size_t slot = 0;
    bool operator==(const LookaheadPreview&) const = default;
  };

  struct LookaheadLoad {
    std::size_t alive = 0;   // controllers not yet freed: shown, kept and retired
    std::size_t running = 0; // listings loading or watching their folder
  };

  // Up to three read-only folder listings ahead of a Workbench list, in the user's sort order and hidden setting.
  // They load on worker jobs through their own controllers and are never the page's controller: nothing here selects,
  // navigates or reads the page's browser. At most kColumns listings run. A listing no longer shown is stopped and
  // kept, at most kSpare of them, so a target that returns reuses its rows; older ones are retired and freed by
  // collectRetired() once the view has let go of their models.
  class LookaheadColumns {
  public:
    static constexpr std::size_t kColumns = 3;
    static constexpr std::size_t kSpare = 3;

    LookaheadColumns(SortOptions sort, bool showHidden, bool showBackups);
    ~LookaheadColumns();
    LookaheadColumns(const LookaheadColumns&) = delete;
    LookaheadColumns& operator=(const LookaheadColumns&) = delete;
    LookaheadColumns(LookaheadColumns&&) = delete;
    LookaheadColumns& operator=(LookaheadColumns&&) = delete;

    // Shows what `request` asks for; an unchanged request changes nothing.
    void follow(const LookaheadRequest& request);
    void setListing(SortOptions sort, bool showHidden, bool showBackups);
    // Stops and retires every listing.
    void clear();
    void collectRetired();

    std::size_t count() const { return m_shown.size(); }
    // The folder and the listing of a shown column, below count().
    const Location& location(std::size_t column) const { return m_shown.at(column).location; }
    const DirectoryModel& model(std::size_t column) const { return m_shown.at(column).browser->model(); }
    const std::optional<LookaheadPreview>& preview() const { return m_preview; }
    LookaheadLoad load() const;

    // Bumped when the columns, the preview or a shown listing change; the callback runs on each bump.
    std::uint64_t serial() const { return m_serial; }
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

  private:
    struct Listing {
      std::unique_ptr<BrowserController> browser;
      Location location;
    };
    // The listing of `folder`: a shown one, a kept one refreshed, or a new one.
    Listing take(const Location& folder);
    // Stops a listing no longer shown and keeps it; past kSpare the oldest kept one is retired.
    void keep(Listing listing);
    void notify();

    std::vector<Listing> m_shown;
    std::vector<Listing> m_spare; // stopped, most recent first
    std::vector<std::unique_ptr<BrowserController>> m_retired;
    std::optional<LookaheadPreview> m_preview;
    SortOptions m_sort;
    bool m_showHidden = false;
    bool m_showBackups = true;
    std::uint64_t m_serial = 0;
    std::function<void()> m_changed;
  };
} // namespace luciole
