#include "browser/measure_cache.h"

#include "fs/background_job.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
  constexpr std::size_t kSearchLevels = 8;

  using Clock = std::chrono::steady_clock;

  struct Entry {
    explicit Entry(luciole::Location folder) : location(std::move(folder)) {}
    luciole::Location location;
    std::shared_ptr<const luciole::MeasureTable> table; // the last complete table, else the newest partial one
    luciole::MeasureHandle walk;
    luciole::MeasurePriority priority = luciole::MeasurePriority::Current;
    luciole::MeasurePriority walked = luciole::MeasurePriority::Current; // the priority the table was walked with
    bool complete = false;
    bool stale = false;
    bool changedDuringWalk = false; // invalidated while a walk ran, so its result is stale on arrival
    bool checking = false;
    std::uint64_t checkSerial = 0;
    Clock::time_point measuredAt;
    Clock::time_point checkedAt;
    mutable std::uint64_t lastUse = 0; // least recently used goes first
  };

  // The record reached from `record` by following child names; kNoMeasureRecord when one is missing.
  std::uint32_t
  descend(const luciole::MeasureTable& table, std::uint32_t record, const std::deque<std::string>& names) {
    for (const auto& name : names) {
      const auto& parent = table.records[record];
      record = luciole::kNoMeasureRecord;
      for (std::uint32_t i = 0; i < parent.childCount; ++i) {
        if (table.records[parent.firstChild + i].name == name) {
          record = parent.firstChild + i;
          break;
        }
      }
      if (record == luciole::kNoMeasureRecord) {
        break;
      }
    }
    return record;
  }

  struct Located {
    std::string uri;
    std::uint32_t record = luciole::kNoMeasureRecord;
  };

  // The nearest of the folder and up to kSearchLevels ancestors whose table records the folder: that table's key and
  // the folder's record in it. `tableOf(uri)` gives the table kept under a key, or null.
  std::optional<Located> locate(const luciole::Location& folder, const auto& tableOf) {
    std::deque<std::string> names;
    std::optional<luciole::Location> level = folder;
    for (std::size_t up = 0; level && up <= kSearchLevels; ++up) {
      if (const luciole::MeasureTable* table = tableOf(level->uri())) {
        const auto record = table->records.empty() ? luciole::kNoMeasureRecord : descend(*table, 0, names);
        if (record != luciole::kNoMeasureRecord) {
          return Located{.uri = level->uri(), .record = record};
        }
      }
      const auto name = level->basename();
      if (!name) {
        break;
      }
      names.push_front(*name);
      level = level->parent();
    }
    return std::nullopt;
  }

  // A table the ancestor budget cut short, asked for by a request that runs without that budget.
  bool cutShort(const Entry& entry, luciole::MeasurePriority priority) {
    return entry.table
        && entry.table->truncated
        && entry.walked == luciole::MeasurePriority::Ancestor
        && priority != luciole::MeasurePriority::Ancestor;
  }

  // What an entry costs against the record budget: a table without records counts as one.
  std::size_t cost(const Entry& entry) {
    return std::max<std::size_t>(1, entry.table ? entry.table->records.size() : 0);
  }
} // namespace

struct luciole::MeasureCache::State : std::enable_shared_from_this<State> {
  State(SubtreeMeasureService& measureService, MeasureCacheOptions cacheOptions)
      : service(measureService), options(cacheOptions) {}

  SubtreeMeasureService& service;
  MeasureCacheOptions options;
  std::unordered_map<std::string, Entry> entries; // by URI
  std::map<std::uint64_t, std::function<void()>> listeners;
  std::uint64_t nextListener = 0;
  mutable std::uint64_t tick = 0;
  mutable std::shared_ptr<const MeasureView> latestView; // null once something it shows changed
  std::unordered_map<std::string, std::size_t> leases;   // by URI, only while at least one is held

  void touched() { latestView.reset(); }

  // The last lease of a folder cancels its walk: the service ends it once no other handle holds it.
  void release(const std::string& uri) {
    const auto held = leases.find(uri);
    if (held == leases.end() || --held->second > 0) {
      return;
    }
    leases.erase(held);
    const auto found = entries.find(uri);
    if (found == entries.end() || !found->second.walk.valid()) {
      return;
    }
    auto& entry = found->second;
    entry.walk.reset();
    entry.changedDuringWalk = false;
    if (!entry.complete) {
      entries.erase(found);
    }
    touched();
  }

  void notify() const {
    const auto callbacks = listeners;
    for (const auto& [id, callback] : callbacks) {
      callback();
    }
  }

  MeasureFind view(const Entry& entry, std::uint32_t record) const {
    entry.lastUse = ++tick;
    return {.table = entry.table, .record = record, .stale = entry.stale, .measuring = entry.walk.valid()};
  }

  void startWalk(Entry& entry, MeasurePriority priority) {
    entry.priority = priority;
    entry.stale = entry.stale || entry.complete;
    entry.walk = service.request(entry.location, priority);
    touched();
  }

  // Compares the folder's stamp with the table's root on a worker; a difference starts a walk.
  void startCheck(Entry& entry) {
    entry.checking = true;
    const auto serial = ++entry.checkSerial;
    runJob<std::optional<DirectoryStamp>>(
        [location = entry.location] { return directoryStamp(location); },
        [weak = weak_from_this(), uri = entry.location.uri(), serial](JobResult<std::optional<DirectoryStamp>> reply) {
          const auto state = weak.lock();
          if (!state) {
            return;
          }
          const auto found = state->entries.find(uri);
          if (found == state->entries.end() || found->second.checkSerial != serial) {
            return;
          }
          auto& entry = found->second;
          entry.checking = false;
          const auto* root = entry.table && !entry.table->records.empty() ? entry.table->records.data() : nullptr;
          if (reply && *reply && root && **reply == root->stamp) {
            entry.checkedAt = Clock::now();
          } else if (!entry.walk.valid()) {
            state->startWalk(entry, entry.priority);
            state->notify();
          }
        }
    );
  }

  // Takes what the service published. Tables only grow in place while no complete table exists.
  void published() {
    bool changed = false;
    for (auto& [uri, entry] : entries) {
      if (!entry.walk.valid()) {
        continue;
      }
      const auto table = entry.walk.table();
      if (entry.walk.done()) {
        entry.table = table;
        entry.walked = entry.walk.walkPriority();
        entry.complete = true;
        entry.stale = std::exchange(entry.changedDuringWalk, false);
        entry.measuredAt = entry.checkedAt = Clock::now();
        entry.walk.reset();
        changed = true;
      } else if (table && !entry.complete && table != entry.table) {
        entry.table = table;
        changed = true;
      }
    }
    evict();
    if (changed) {
      touched();
      notify();
    }
  }

  std::size_t recordCount() const {
    std::size_t total = 0;
    for (const auto& [uri, entry] : entries) {
      total += cost(entry);
    }
    return total;
  }

  // Drops least recently used tables over the budget, never the most recent one. Bounded by the entry count.
  void evict() {
    for (auto total = recordCount(); total > options.recordBudget && entries.size() > 1;) {
      const auto newest = std::ranges::max_element(entries, {}, [](const auto& item) { return item.second.lastUse; });
      auto oldest = entries.end();
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it != newest && (oldest == entries.end() || it->second.lastUse < oldest->second.lastUse)) {
          oldest = it;
        }
      }
      total -= cost(oldest->second);
      entries.erase(oldest);
      touched();
    }
  }
};

luciole::MeasureCache::MeasureCache(SubtreeMeasureService& service, MeasureCacheOptions options)
    : m_state(std::make_shared<State>(service, options)) {
  service.setChangedCallback([weak = std::weak_ptr<State>(m_state)] {
    if (const auto state = weak.lock()) {
      state->published();
    }
  });
}

luciole::MeasureCache::~MeasureCache() {
  m_state->service.setChangedCallback({});
  m_state->listeners.clear();
}

luciole::MeasureFind luciole::MeasureCache::measure(const Location& folder, MeasurePriority priority) {
  auto& state = *m_state;
  auto [found, inserted] = state.entries.try_emplace(folder.uri(), folder);
  auto& entry = found->second;
  const auto now = Clock::now();
  if (entry.walk.valid()) {
    if (priority < entry.priority) {
      // A second request shares the walk and raises its priority; the first handle then goes.
      entry.priority = priority;
      entry.walk = state.service.request(entry.location, priority);
    }
  } else if (
      inserted
      || entry.stale
      || !entry.complete
      || cutShort(entry, priority)
      || now - entry.measuredAt >= state.options.maxAge
  ) {
    state.startWalk(entry, priority);
  } else if (!entry.checking && now - entry.checkedAt >= state.options.checkAge) {
    entry.priority = priority;
    state.startCheck(entry);
  }
  return state.view(entry, entry.table && !entry.table->records.empty() ? 0 : kNoMeasureRecord);
}

luciole::MeasureFind luciole::MeasureCache::find(const Location& folder) const {
  const auto& state = *m_state;
  const auto found = locate(folder, [&state](const std::string& uri) -> const MeasureTable* {
    const auto entry = state.entries.find(uri);
    return entry != state.entries.end() ? entry->second.table.get() : nullptr;
  });
  return found ? state.view(state.entries.at(found->uri), found->record) : MeasureFind{};
}

std::shared_ptr<const luciole::MeasureView> luciole::MeasureCache::view() const {
  const auto& state = *m_state;
  if (!state.latestView) {
    auto view = std::make_shared<MeasureView>();
    view->m_tables.reserve(state.entries.size());
    for (const auto& [uri, entry] : state.entries) {
      if (entry.table) {
        view->m_tables.emplace(
            uri, MeasureFind{.table = entry.table, .record = 0, .stale = entry.stale, .measuring = entry.walk.valid()}
        );
      }
    }
    state.latestView = std::move(view);
  }
  return state.latestView;
}

luciole::MeasureFind luciole::MeasureView::find(const Location& folder) const {
  const auto found = locate(folder, [this](const std::string& uri) -> const MeasureTable* {
    const auto table = m_tables.find(uri);
    return table != m_tables.end() ? table->second.table.get() : nullptr;
  });
  if (!found) {
    return {};
  }
  auto result = m_tables.at(found->uri);
  result.record = found->record;
  return result;
}

void luciole::MeasureCache::invalidate(const Location& location) {
  bool changed = false;
  for (auto& [uri, entry] : m_state->entries) {
    if (entry.location == location || location.isInside(entry.location)) {
      changed = changed || !entry.stale;
      entry.stale = true;
      entry.changedDuringWalk = entry.walk.valid();
    }
  }
  if (changed) {
    m_state->touched();
    m_state->notify();
  }
}

luciole::MeasureCache::Lease luciole::MeasureCache::lease(const Location& folder) {
  Lease lease;
  lease.m_state = m_state;
  lease.m_uri = folder.uri();
  ++m_state->leases[lease.m_uri];
  return lease;
}

luciole::MeasureCache::Lease::~Lease() { reset(); }

luciole::MeasureCache::Lease::Lease(Lease&& other) noexcept
    : m_state(std::move(other.m_state)), m_uri(std::exchange(other.m_uri, {})) {}

luciole::MeasureCache::Lease& luciole::MeasureCache::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    m_state = std::move(other.m_state);
    m_uri = std::exchange(other.m_uri, {});
  }
  return *this;
}

void luciole::MeasureCache::Lease::reset() {
  if (m_uri.empty()) {
    return;
  }
  if (const auto state = m_state.lock()) {
    state->release(m_uri);
  }
  m_uri.clear();
  m_state.reset();
}

std::uint64_t luciole::MeasureCache::addListener(std::function<void()> changed) {
  const auto id = ++m_state->nextListener;
  m_state->listeners.emplace(id, std::move(changed));
  return id;
}

void luciole::MeasureCache::removeListener(std::uint64_t id) { m_state->listeners.erase(id); }

std::size_t luciole::MeasureCache::tables() const { return m_state->entries.size(); }

std::size_t luciole::MeasureCache::records() const { return m_state->recordCount(); }
