#pragma once

#include "browser/measure_cache.h"
#include "browser/workbench_services.h"
#include "browser/workbench_snapshot.h"
#include "config/visit_store.h"
#include "workbench/jump_rank.h"
#include "workbench/measure.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace luciole {
  // Every visit at or below a folder, ordered by URI. Called on the main context.
  using WorkbenchVisits = std::function<std::vector<VisitRecord>(const Location& folder)>;

  struct WorkbenchModelOptions {
    std::size_t ancestorLevels = 64; // ancestors shown and measured, nearest first; at most kMaxAncestors
  };

  // The measured picture of one Workbench page. Its WorkbenchWalks ask the shared cache for the current folder, whose
  // walk records its children and the two levels below them, any child invalidated since, the folders of the Jump
  // ahead rows, then each ancestor under the service's ancestor budget. Ancestors are listed on a worker meanwhile,
  // so their entries show before their measures. New tables become a new snapshot at most once per
  // kPublishInterval. A worker builds the trees from the cache's view, a refreshed child's only when its table
  // changed, then grafts, sets visit counts, ranks and computes shares; the main context hands it pointers and
  // publishing swaps one. Main context only.
  class WorkbenchModel {
  public:
    static constexpr std::chrono::milliseconds kPublishInterval{100};
    static constexpr std::size_t kJumpWalks = workbench::kJumpRows; // Jump ahead folders measured per folder
    static constexpr std::size_t kMaxAncestors = 64;

    explicit WorkbenchModel(MeasureCache& cache, WorkbenchModelOptions options = {});
    // Never blocks: walks no other page holds are cancelled, a running build or listing is discarded. It may be
    // called from the changed callback; nothing starts afterwards.
    ~WorkbenchModel();
    WorkbenchModel(const WorkbenchModel&) = delete;
    WorkbenchModel& operator=(const WorkbenchModel&) = delete;

    // The page's folder; the same folder again changes nothing.
    void setLocation(const Location& location);
    // Settings and visits finish the last tree again on a worker; the tree is not built again for them.
    void setSettings(const workbench::LayoutSettings& settings);
    // Reads the visits below the outermost folder shown now and whenever the folder changes; call it again when the
    // visits change.
    void setVisits(WorkbenchVisits visits);
    // The folder's listing changed at `name`: a folder child is walked again alone, and one that is gone leaves the
    // picture. The whole folder is walked again instead for a file child, since only the folder's walk measures its
    // files, for a child the folder's table may have folded into its remainder, and once kRefreshedChildren
    // children were walked alone. A local folder is watched, and what changes in it arrives here at most once per
    // kPublishInterval; a change of the folder itself, or of more entries than that, walks the whole folder again.
    // A folder or child walked before is walked again no sooner than its WorkbenchWalks due time.
    void invalidateChild(const std::string& name);
    // The user asked to reload: the folder is measured again, at once and not for a changed entry alone. The numbers
    // shown stay until the new ones arrive; a request while the walk runs makes at most one more walk.
    void remeasure();
    // The drive at `root` is being removed: no walk of it or of a folder below it runs or starts.
    void releaseLocation(const Location& root);

    // The latest picture, null before the first one. Its location says which folder it shows: after setLocation() it
    // is the previous folder's until the new folder's first measure, or its error, arrives.
    std::shared_ptr<const WorkbenchSnapshot> snapshot() const;
    // The serial of snapshot(); 0 before the first one.
    std::uint64_t serial() const;
    // True while the folder's walk or a refreshed child's has not finished, the ancestors are being listed, or newer
    // tables or settings have not been published yet. Jump ahead and ancestor walks only refine the picture.
    bool measuring() const;
    // Runs on the main context when a snapshot is published or measuring() changes.
    void setChangedCallback(std::function<void()> changed);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
