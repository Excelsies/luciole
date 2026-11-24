#pragma once

#include "fs/measure_amend.h"
#include "fs/subtree_measure.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace luciole {
  // A folder's record in a cached table: the root of its own table, or a record inside a cached ancestor's table.
  struct MeasureFind {
    std::shared_ptr<const MeasureTable> table;
    std::uint32_t record = kNoMeasureRecord;
    bool stale = false;     // the folder changed or the table aged; a new walk will replace it
    bool measuring = false; // a walk of the table's root is queued or running
    // Null when nothing is known.
    const MeasureRecord* entry() const {
      return table && record < table->records.size() ? &table->records[record] : nullptr;
    }
  };

  // The cache's tables and flags at one moment. Immutable, so a worker may read it while the cache moves on.
  class MeasureView {
  public:
    // Known measure only, from the folder's own table or one of up to eight ancestors' tables.
    MeasureFind find(const Location& folder) const;

  private:
    friend class MeasureCache;
    std::unordered_map<std::string, MeasureFind> m_tables; // by URI, each with record 0
  };

  struct MeasureCacheOptions {
    std::size_t recordBudget = 250'000;       // records kept over every cached table
    std::chrono::milliseconds checkAge{5000}; // compare the folder's stamp when the table is older than this
    std::chrono::milliseconds maxAge{60000};  // walk again when the table is older than this
  };

  // Folder measures shared by tabs and windows, keyed by location. Main context only: it never touches the disk
  // itself, stamp checks run on the worker pool and walks in the measure service, which must outlive the cache.
  // The cache takes the service's changed callback.
  class MeasureCache {
    struct State;

  public:
    // Says a folder's walk is still wanted. When the last lease of a folder goes, its unfinished walk is cancelled
    // and a folder without a complete table is forgotten, without notifying listeners. Only that release cancels: a
    // walk started while no lease is held runs to its end.
    class [[nodiscard]] Lease {
    public:
      Lease() = default;
      ~Lease();
      Lease(Lease&& other) noexcept;
      Lease& operator=(Lease&& other) noexcept;
      Lease(const Lease&) = delete;
      Lease& operator=(const Lease&) = delete;

      bool valid() const { return !m_uri.empty(); }
      void reset();

    private:
      friend class MeasureCache;
      std::weak_ptr<State> m_state;
      std::string m_uri;
    };

    explicit MeasureCache(SubtreeMeasureService& service, MeasureCacheOptions options = {});
    ~MeasureCache();
    MeasureCache(const MeasureCache&) = delete;
    MeasureCache& operator=(const MeasureCache&) = delete;

    // What is known about the folder now. Walks when nothing is known, after invalidate() or a changed stamp, when
    // the table is older than maxAge, or when the ancestor budget truncated it and a more urgent request asks;
    // checks the stamp when it is older than checkAge. The last complete table is served, flagged stale, until the
    // new walk finishes.
    MeasureFind measure(const Location& folder, MeasurePriority priority);
    // Known measure only, from the folder's own table or one of up to eight ancestors' tables.
    MeasureFind find(const Location& folder) const;
    // The tables as they are now; the same view until a table, a stale flag or a walk changes.
    std::shared_ptr<const MeasureView> view() const;
    // Marks the location and every cached ancestor stale; for change notifications.
    void invalidate(const Location& location);
    // Replaces the folder's complete table `from`, as long as it is the one served, not stale and not being walked
    // again, by a copy with `amendments` applied, and returns that copy; listeners hear of it. Null when it was not
    // replaced, also when no amendment applied.
    std::shared_ptr<const MeasureTable> amend(
        const Location& folder, const std::shared_ptr<const MeasureTable>& from,
        std::span<const MeasureAmendment> amendments
    );
    // A lease on the folder; taking one starts no walk.
    Lease lease(const Location& folder);

    // Listeners run on the main context when tables or stale flags change.
    [[nodiscard]] std::uint64_t addListener(std::function<void()> changed);
    void removeListener(std::uint64_t id);

    std::size_t tables() const;
    // Records held against the budget; a table without records counts as one.
    std::size_t records() const;

  private:
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
