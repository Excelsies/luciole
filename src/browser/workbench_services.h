#pragma once

#include "browser/measure_cache.h"
#include "fs/subtree_measure.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glib.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace luciole {
  class TextTileService;
  class ThumbnailService;

  // The preview workers of the modules above this one, which the application owns beside these services and lends
  // to every page's column previews. Null until the application sets them.
  struct WorkbenchPreviewWorkers {
    TextTileService* text = nullptr;
    ThumbnailService* thumbnails = nullptr;
  };

  // The folder measures every Workbench page of the application shares: one measure service and the cache over it.
  // Main context only; destroying it cancels running walks and waits for their threads.
  class WorkbenchServices {
  public:
    static constexpr std::size_t kWalks = 2;              // walks running at once
    static constexpr std::size_t kRecordBudget = 250'000; // records the cache keeps over every table

    WorkbenchServices();
    WorkbenchServices(const WorkbenchServices&) = delete;
    WorkbenchServices& operator=(const WorkbenchServices&) = delete;

    SubtreeMeasureService& measureService() { return m_service; }
    MeasureCache& measureCache() { return m_cache; }
    // Borrowed; they must outlive every page.
    void setPreviewWorkers(WorkbenchPreviewWorkers workers) { m_previewWorkers = workers; }
    const WorkbenchPreviewWorkers& previewWorkers() const { return m_previewWorkers; }

  private:
    SubtreeMeasureService m_service; // declared first: the cache must go before it
    MeasureCache m_cache;
    WorkbenchPreviewWorkers m_previewWorkers;
  };

  // The walks one Workbench page asks of the shared cache, started in this order: the folder, then its invalidated
  // children, then Jump ahead folders once the folder's walk ended, then the ancestors, nearest first, once the Jump
  // folders were planned. At most kWalks are queued or running at once. Each started walk holds a cache lease, so a
  // walk the page drops, by leaving its folder or by going, is cancelled unless another page holds it too. A walk is
  // not started again because it ended stale, unless its own folder was invalidated while it ran. A walk asked again
  // starts no sooner than kAgainGap after its last one ended, nor sooner than kAgainFactor times as long as that one
  // took, so a folder that keeps changing is walked a few times, not back to back. A walk without the ancestor budget
  // that ends with a table that budget cut short is walked again in full. Main context only.
  class WorkbenchWalks {
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t kWalks = 2;
    static constexpr std::size_t kRefreshedChildren = 16; // children walked again alone; one more walks the folder
    static constexpr std::chrono::milliseconds kAgainGap{1000};
    static constexpr int kAgainFactor = 4;

    enum class Want : std::uint8_t { Folder, Child, Jump, Ancestor };

    struct Walk {
      Location location;
      Want want = Want::Folder;
      std::string name; // the child's name for Want::Child
      MeasureFind found;
      bool started = false;
      bool finished = false;
      bool again = false;          // its folder changed while it ran: walked again once it ends
      Clock::time_point startedAt; // when its last walk was asked for
      Clock::time_point due;       // when it may be walked again
      MeasureCache::Lease lease;
    };

    // `wake` runs on the main context once a walk held back until its due time may start; the owner then calls
    // update(). It may destroy the walks.
    WorkbenchWalks(MeasureCache& cache, std::function<void()> wake);
    ~WorkbenchWalks();
    WorkbenchWalks(const WorkbenchWalks&) = delete;
    WorkbenchWalks& operator=(const WorkbenchWalks&) = delete;

    // Starts over at `folder` with its ancestors, nearest first. A started walk the new folder also wants goes on for
    // it; the other walks of the folder left are dropped.
    void enter(const Location& folder, std::span<const Location> ancestors);
    // Adds Jump ahead folders until `limit` Jump walks are planned; ancestors may start from now on.
    void planJumps(std::span<const Location> folders, std::size_t limit);
    // The folder's child `name` changed: it is walked again alone, or the whole folder is once kRefreshedChildren
    // children were.
    void refreshChild(const std::string& name);
    // Drops the refreshed children and walks the folder again: at once when its walk ended, else once it ends.
    void walkFolderAgain();
    // Reads the started walks from the cache and starts what may start. The folder is walked again when a child
    // turned out to be a file, or when its finished table folds folders into a remainder and does not record a
    // refreshed child, since only the folder's walk measures those.
    void update();

    std::span<const Walk> walks() const { return m_walks; }
    const Walk* folder() const;
    bool folderFinished() const;
    bool jumpsPlanned() const { return m_jumpsPlanned; }
    // The folder's walk or a refreshed child's has not finished.
    bool waiting() const;

  private:
    void poll();
    void ask(Walk& walk, Clock::time_point now);
    std::size_t running() const;
    Walk* next(Clock::time_point now);
    void start();
    void wakeForDue(Clock::time_point now);
    void stopTimer();
    bool folderMeasuresChild() const;

    MeasureCache& m_cache;
    std::function<void()> m_wake;
    std::vector<Walk> m_walks; // for the current folder
    bool m_jumpsPlanned = false;
    GSource* m_timer = nullptr; // runs m_wake at m_wakeAt
    Clock::time_point m_wakeAt;
  };

  // The direct entries of a page's folder that change on disk, handed over at most once per interval: their names,
  // or only `whole` once the folder itself changed or more than kNames entries did. A folder that is not local, or
  // that cannot be watched, reports nothing. Main context only; nothing runs while the folder is quiet.
  class FolderChanges {
  public:
    static constexpr std::size_t kNames = WorkbenchWalks::kRefreshedChildren;
    using Changed = std::function<void(std::span<const std::string> names, bool whole)>;

    explicit FolderChanges(std::chrono::milliseconds interval);
    ~FolderChanges();
    FolderChanges(const FolderChanges&) = delete;
    FolderChanges& operator=(const FolderChanges&) = delete;

    // Watches `folder` from now on; what the previous folder collected is dropped.
    void watch(const Location& folder);
    void stop();
    void setChangedCallback(Changed changed);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
