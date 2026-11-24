#pragma once

#include "browser/measure_cache.h"
#include "fs/measure_amend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace luciole {
  struct UnreachedLimits {
    std::size_t children = 1000;
    std::uint64_t entries = 2'000'000;
    // Sizes reach the table in batches, at most this often; the last batch at once.
    std::chrono::milliseconds applyEvery{100};
  };

  // The second pass of a folder's measure. When the walk of a folder ended with its entry budget used up, the folders
  // it listed and never entered have no size. The pass walks each of them alone, with the walk's own budget, and puts
  // the sizes into the folder's table in the cache, so every reader of the table, the Size cells and the picture, takes
  // them together. A folder that is itself cut by its budget ends as a lower bound; a folder the first walk entered is
  // never walked again. At most `children` folders are walked, and no further one starts once they counted `entries`
  // entries; a folder in flight when that happens ends first, so the pass stays within the entry budget plus the
  // budget of each walk running at once. What it leaves out stays never entered. Main context only.
  class UnreachedPass {
  public:
    explicit UnreachedPass(MeasureCache& cache, UnreachedLimits limits = {});
    ~UnreachedPass();
    UnreachedPass(const UnreachedPass&) = delete;
    UnreachedPass& operator=(const UnreachedPass&) = delete;

    // Drops the plan: running walks are cancelled and sizes not yet applied are lost.
    void reset();
    // `table` is the finished, not stale table of `folder`, or null when there is none: another table than the
    // planned one is a new plan, and none drops the plan. Takes the sizes of finished walks, applies them, and starts
    // walks while fewer than `slots` run.
    void update(const Location& folder, const std::shared_ptr<const MeasureTable>& table, std::size_t slots);

    std::size_t running() const { return m_running.size(); }
    // Folders still to walk, walking, or walked and not applied yet.
    bool pending() const { return !m_queue.empty() || !m_running.empty() || !m_batch.empty(); }

  private:
    struct Child {
      std::string name;
      Location location;
      MeasureFind found;
      MeasureCache::Lease lease;
    };

    void plan(const std::shared_ptr<const MeasureTable>& table);
    void harvest();
    void apply(const Location& folder, bool last);
    void start(const Location& folder, std::size_t slots);

    MeasureCache& m_cache;
    UnreachedLimits m_limits;
    std::shared_ptr<const MeasureTable> m_table; // what the plan was made for, then what the last batch made
    std::deque<std::string> m_queue;
    std::vector<Child> m_running;
    std::vector<MeasureAmendment> m_batch;
    std::uint64_t m_entries = 0;
    std::chrono::steady_clock::time_point m_applied;
  };
} // namespace luciole
