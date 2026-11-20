#pragma once

#include "fs/subtree_measure_walk.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace luciole {
  // Lower values run first; equal priorities run in request order.
  enum class MeasurePriority : std::uint8_t { Current, Ahead, Jump, Ancestor };

  struct MeasureServiceOptions {
    MeasureOptions walk; // caps and budget of every walk; its deadline, interval and token are set per walk
    std::uint64_t ancestorBudget = 200'000;         // entry budget of an Ancestor walk
    std::chrono::milliseconds ancestorTime{2000};   // time budget of an Ancestor walk
    std::chrono::milliseconds publishInterval{100}; // partial tables and main-context publication
    std::size_t workers = 2;                        // walks at once, clamped to 1..2
    // For tests: runs on the worker thread after each partial table of a walk is stored; the walk waits for it.
    std::function<void()> afterPartial;
  };

  struct MeasureServiceStatus {
    std::size_t queued = 0;
    std::size_t running = 0;
    bool operator==(const MeasureServiceStatus&) const = default;
  };

  namespace detail {
    struct MeasureJob;
    struct MeasureServiceState;
  } // namespace detail

  // Keeps one request alive. Dropping the last handle of a walk that has not finished cancels it.
  class [[nodiscard]] MeasureHandle {
  public:
    MeasureHandle() = default;
    ~MeasureHandle();
    MeasureHandle(MeasureHandle&& other) noexcept;
    MeasureHandle& operator=(MeasureHandle&& other) noexcept;
    MeasureHandle(const MeasureHandle&) = delete;
    MeasureHandle& operator=(const MeasureHandle&) = delete;

    bool valid() const { return m_job != nullptr; }
    // The latest published table: null before the first publication, partial while the walk runs.
    std::shared_ptr<const MeasureTable> table() const;
    // True once the final table is published.
    bool done() const;
    // The priority the published table was walked with; an Ancestor walk runs under the ancestor budget.
    MeasurePriority walkPriority() const;
    void reset();

  private:
    friend class SubtreeMeasureService;
    std::shared_ptr<detail::MeasureJob> m_job;
    std::weak_ptr<detail::MeasureServiceState> m_service;
  };

  // Measures folders on worker threads of its own, so long walks never occupy the shared pool that listings and
  // thumbnails use. Requests and publication belong to the default main context.
  class SubtreeMeasureService {
  public:
    explicit SubtreeMeasureService(MeasureServiceOptions options = {});
    // Cancels running walks and waits for their threads.
    ~SubtreeMeasureService();
    SubtreeMeasureService(const SubtreeMeasureService&) = delete;
    SubtreeMeasureService& operator=(const SubtreeMeasureService&) = delete;

    // Shares a queued or running walk of the same location, raising its priority when this one is more urgent.
    // A finished walk is never shared: a later request walks again.
    MeasureHandle request(const Location& root, MeasurePriority priority);
    MeasureServiceStatus status() const;
    // A walk of `root` or of a folder below it is queued or still reading, cancelled or not.
    bool reading(const Location& root) const;
    // Runs on the main context after new tables were published, at most once per publish interval.
    void setChangedCallback(std::function<void()> changed);

  private:
    std::shared_ptr<detail::MeasureServiceState> m_state;
  };
} // namespace luciole
