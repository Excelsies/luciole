#include "browser/workbench_services.h"

#include "fs/directory_watch.h"
#include "fs/location_within.h"

#include <algorithm>
#include <filesystem>
#include <glib.h>
#include <optional>
#include <utility>

namespace {
  using luciole::MeasurePriority;
  using luciole::MeasureTable;
  using Want = luciole::WorkbenchWalks::Want;
  using Walk = luciole::WorkbenchWalks::Walk;

  luciole::MeasureServiceOptions serviceOptions() {
    luciole::MeasureServiceOptions options;
    options.workers = luciole::WorkbenchServices::kWalks;
    return options;
  }

  luciole::MeasureCacheOptions cacheOptions() {
    luciole::MeasureCacheOptions options;
    options.recordBudget = luciole::WorkbenchServices::kRecordBudget;
    return options;
  }

  MeasurePriority priorityOf(Want want) {
    switch (want) {
    case Want::Folder:
    case Want::Child:
      return MeasurePriority::Current;
    case Want::Jump:
      return MeasurePriority::Jump;
    case Want::Ancestor:
      return MeasurePriority::Ancestor;
    }
    return MeasurePriority::Ancestor;
  }

  bool walking(const Walk& walk) { return walk.started && !walk.finished; }

  // A fresh truncated table that a walk without the ancestor budget asks for; measure() walks it again in full only
  // when the ancestor budget cut it short.
  bool cutShort(const Walk& walk) {
    const auto* table = walk.found.table.get();
    return walk.want != Want::Ancestor && table && table->truncated && !walk.found.stale;
  }

  // Whether the table's root records a child folder of this name; bounded by its child records.
  bool recordsChild(const MeasureTable& table, const std::string& name) {
    const auto& root = table.records[0];
    for (auto c = root.firstChild; c < root.firstChild + root.childCount && c < table.records.size(); ++c) {
      if (table.records[c].name == name) {
        return true;
      }
    }
    return false;
  }

} // namespace

luciole::WorkbenchServices::WorkbenchServices() : m_service(serviceOptions()), m_cache(m_service, cacheOptions()) {}

luciole::WorkbenchWalks::WorkbenchWalks(MeasureCache& cache, std::function<void()> wake)
    : m_cache(cache), m_wake(std::move(wake)) {}

luciole::WorkbenchWalks::~WorkbenchWalks() { stopTimer(); }

// Bounded by the new walks times the walks left.
void luciole::WorkbenchWalks::enter(const Location& folder, std::span<const Location> ancestors) {
  stopTimer();
  auto left = std::exchange(m_walks, {});
  m_jumpsPlanned = false;
  m_walks.push_back(
      {.location = folder, .want = Want::Folder, .name = {}, .found = {}, .startedAt = {}, .due = {}, .lease = {}}
  );
  for (const auto& ancestor : ancestors) {
    m_walks.push_back(
        {.location = ancestor, .want = Want::Ancestor, .name = {}, .found = {}, .startedAt = {}, .due = {}, .lease = {}}
    );
  }
  for (auto& walk : m_walks) {
    const auto kept =
        std::ranges::find_if(left, [&walk](const Walk& old) { return walking(old) && old.location == walk.location; });
    if (kept != left.end()) {
      walk.found = std::move(kept->found);
      walk.lease = std::move(kept->lease);
      walk.started = true;
      walk.again = kept->again;
      walk.startedAt = kept->startedAt;
      left.erase(kept);
    }
  }
}

void luciole::WorkbenchWalks::planJumps(std::span<const Location> folders, std::size_t limit) {
  m_jumpsPlanned = true;
  for (const auto& folder : folders) {
    if (static_cast<std::size_t>(std::ranges::count(m_walks, Want::Jump, &Walk::want)) >= limit) {
      break;
    }
    if (!std::ranges::contains(m_walks, folder, &Walk::location)) {
      m_walks.push_back(
          {.location = folder, .want = Want::Jump, .name = {}, .found = {}, .startedAt = {}, .due = {}, .lease = {}}
      );
    }
  }
}

void luciole::WorkbenchWalks::refreshChild(const std::string& name) {
  const auto* current = folder();
  if (current == nullptr || name.empty()) {
    return;
  }
  const auto child = current->location.child(name);
  const auto known = std::ranges::find_if(m_walks, [&child](const Walk& walk) {
    return walk.want == Want::Child && walk.location == child;
  });
  if (known == m_walks.end()) {
    if (static_cast<std::size_t>(std::ranges::count(m_walks, Want::Child, &Walk::want)) >= kRefreshedChildren) {
      walkFolderAgain();
      return;
    }
    m_walks.push_back(
        {.location = child, .want = Want::Child, .name = name, .found = {}, .startedAt = {}, .due = {}, .lease = {}}
    );
  } else if (known->finished) {
    known->started = false;
    known->finished = false;
  } else {
    known->again = known->started;
  }
  m_cache.invalidate(child);
}

void luciole::WorkbenchWalks::walkFolderAgain() {
  std::erase_if(m_walks, [](const Walk& walk) { return walk.want == Want::Child; });
  if (const auto current = std::ranges::find(m_walks, Want::Folder, &Walk::want); current != m_walks.end()) {
    const bool running = walking(*current);
    current->again = running;
    current->started = running;
    current->finished = false;
    m_cache.invalidate(current->location);
  }
}

void luciole::WorkbenchWalks::clear() {
  stopTimer();
  m_walks.clear();
  m_jumpsPlanned = false;
}

void luciole::WorkbenchWalks::release(const Location& root) {
  std::erase_if(m_walks, [&root](const Walk& walk) { return locationWithin(walk.location, root); });
}

void luciole::WorkbenchWalks::update() {
  poll();
  if (folderMeasuresChild()) {
    walkFolderAgain();
  }
  start();
}

const luciole::WorkbenchWalks::Walk* luciole::WorkbenchWalks::folder() const {
  const auto found = std::ranges::find(m_walks, Want::Folder, &Walk::want);
  return found != m_walks.end() ? &*found : nullptr;
}

bool luciole::WorkbenchWalks::folderFinished() const {
  const auto* current = folder();
  return current && current->finished;
}

bool luciole::WorkbenchWalks::waiting() const {
  return std::ranges::any_of(m_walks, [](const Walk& walk) {
    return (walk.want == Want::Folder || walk.want == Want::Child) && !walk.finished;
  });
}

// A table of the folder's own is read with find(), so a walk that ended stale is not started again; only a folder
// without one is asked with measure(), which shares its queued walk. A walk asked again once it ends waits for its
// due time.
void luciole::WorkbenchWalks::poll() {
  const auto now = Clock::now();
  for (auto& walk : m_walks) {
    if (!walking(walk)) {
      continue;
    }
    auto found = m_cache.find(walk.location);
    if (found.record != 0) {
      found = m_cache.measure(walk.location, priorityOf(walk.want));
    }
    walk.found = std::move(found);
    walk.finished = !walk.found.measuring;
    if (!walk.finished) {
      continue;
    }
    walk.due = now + std::max<Clock::duration>(kAgainGap, (now - walk.startedAt) * kAgainFactor);
    if (std::exchange(walk.again, false) && walk.found.stale) {
      walk.started = false;
      walk.finished = false;
    } else if (cutShort(walk)) {
      ask(walk, now);
    }
  }
}

void luciole::WorkbenchWalks::ask(Walk& walk, Clock::time_point now) {
  walk.startedAt = now;
  walk.found = m_cache.measure(walk.location, priorityOf(walk.want));
  walk.finished = !walk.found.measuring;
}

std::size_t luciole::WorkbenchWalks::running() const {
  return static_cast<std::size_t>(std::ranges::count_if(m_walks, walking));
}

// The most urgent walk not started yet that is due and may start now.
luciole::WorkbenchWalks::Walk* luciole::WorkbenchWalks::next(Clock::time_point now) {
  Walk* best = nullptr;
  for (auto& walk : m_walks) {
    if (!walk.started && walk.due <= now && (best == nullptr || walk.want < best->want)) {
      best = &walk;
    }
  }
  if (best == nullptr
      || (best->want == Want::Jump && !folderFinished())
      || (best->want == Want::Ancestor && (!folderFinished() || !m_jumpsPlanned))) {
    return nullptr;
  }
  return best;
}

// Bounded by the walk count.
void luciole::WorkbenchWalks::start() {
  const auto now = Clock::now();
  for (std::size_t step = 0; step < m_walks.size() && running() < kWalks; ++step) {
    auto* walk = next(now);
    if (walk == nullptr) {
      break;
    }
    walk->lease = m_cache.lease(walk->location);
    ask(*walk, now);
    walk->started = true;
  }
  wakeForDue(now);
}

// Keeps a timer for the earliest walk held back until its due time, or none.
void luciole::WorkbenchWalks::wakeForDue(Clock::time_point now) {
  std::optional<Clock::time_point> earliest;
  for (const auto& walk : m_walks) {
    if (!walk.started && walk.due > now && (!earliest || walk.due < *earliest)) {
      earliest = walk.due;
    }
  }
  if (!earliest) {
    stopTimer();
    return;
  }
  if (m_timer != nullptr && m_wakeAt <= *earliest) {
    return;
  }
  stopTimer();
  m_wakeAt = *earliest;
  m_timer =
      g_timeout_source_new(static_cast<guint>(std::chrono::ceil<std::chrono::milliseconds>(*earliest - now).count()));
  g_source_set_callback(
      m_timer,
      [](gpointer data) -> gboolean {
        auto* owner = static_cast<WorkbenchWalks*>(data);
        g_source_unref(std::exchange(owner->m_timer, nullptr));
        // The owner may destroy the walks from the callback.
        const auto wake = owner->m_wake;
        if (wake) {
          wake();
        }
        return G_SOURCE_REMOVE;
      },
      this, nullptr
  );
  g_source_attach(m_timer, g_main_context_default());
}

void luciole::WorkbenchWalks::stopTimer() {
  if (auto* source = std::exchange(m_timer, nullptr)) {
    g_source_destroy(source);
    g_source_unref(source);
  }
}

bool luciole::WorkbenchWalks::folderMeasuresChild() const {
  const auto* current = folder();
  const auto* table = current && current->finished ? current->found.table.get() : nullptr;
  const bool folds = table && !table->records.empty() && table->records[0].otherDirectories.count > 0;
  return std::ranges::any_of(m_walks, [table, folds](const Walk& walk) {
    if (walk.want != Want::Child) {
      return false;
    }
    const auto* own = walk.found.table.get();
    const bool file = walk.finished && own && own->error && own->error->kind == FileErrorKind::NotDirectory;
    return file || (folds && !recordsChild(*table, walk.name));
  });
}

struct luciole::FolderChanges::State : std::enable_shared_from_this<State> {
  explicit State(std::chrono::milliseconds wait) : interval(wait) {}
  ~State() { clear(); }
  State(const State&) = delete;
  State& operator=(const State&) = delete;

  std::chrono::milliseconds interval;
  Changed changed;
  std::unique_ptr<DirectoryWatch> watch;
  std::filesystem::path folder;
  std::vector<std::string> names; // at most kNames + 1
  bool whole = false;
  GSource* timer = nullptr;

  void clear() {
    watch.reset();
    names.clear();
    whole = false;
    if (auto* source = std::exchange(timer, nullptr)) {
      g_source_destroy(source);
      g_source_unref(source);
    }
  }

  void collect(const std::filesystem::path& path) {
    if (path == folder) {
      whole = true;
    } else if (path.parent_path() == folder) {
      if (auto name = path.filename().string(); !whole && !std::ranges::contains(names, name)) {
        names.push_back(std::move(name));
        whole = names.size() > kNames;
      }
    } else {
      return;
    }
    if (timer == nullptr) {
      schedule();
    }
  }

  void schedule() {
    timer = g_timeout_source_new(static_cast<guint>(std::max<std::int64_t>(1, interval.count())));
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          const auto state = static_cast<std::weak_ptr<State>*>(data)->lock();
          if (state && state->timer != nullptr) {
            g_source_unref(std::exchange(state->timer, nullptr));
            state->handOver();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
    g_source_attach(timer, g_main_context_default());
  }

  void handOver() {
    auto taken = std::exchange(names, {});
    const bool all = std::exchange(whole, false);
    if (all) {
      taken.clear();
    }
    const auto callback = changed;
    if (callback) {
      callback(taken, all);
    }
  }
};

luciole::FolderChanges::FolderChanges(std::chrono::milliseconds interval)
    : m_state(std::make_shared<State>(interval)) {}

luciole::FolderChanges::~FolderChanges() { stop(); }

void luciole::FolderChanges::watch(const Location& folder) {
  auto& state = *m_state;
  state.clear();
  const auto path = folder.nativePath();
  if (!path) {
    return;
  }
  state.folder = *path;
  state.watch = DirectoryWatch::start(state.folder, [weak = std::weak_ptr<State>(m_state)](const auto& changed) {
    if (const auto owner = weak.lock()) {
      owner->collect(changed);
    }
  });
}

void luciole::FolderChanges::stop() {
  m_state->clear();
  m_state->changed = {};
}

void luciole::FolderChanges::setChangedCallback(Changed changed) { m_state->changed = std::move(changed); }
