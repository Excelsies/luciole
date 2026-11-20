#include "config/visit_service.h"

#include "fs/background_job.h"

#include <algorithm>
#include <cstdint>
#include <glib.h>
#include <print>
#include <utility>

namespace {
  using luciole::Visit;
  using luciole::VisitRecord;
  using luciole::VisitStore;
  using Part = VisitStore::Part;

  // Distinct unsaved locations that start a write without waiting for a pause.
  constexpr std::size_t kPendingLimit = 256;
  // About one frame: recorded visits are announced this long after the first of a burst, once for all of it.
  constexpr guint kNoticeMs = 16;

  void destroySource(GSource*& source) {
    if (auto* owned = std::exchange(source, nullptr)) {
      g_source_destroy(owned);
      g_source_unref(owned);
    }
  }

  void merge(Visit& into, Visit from) { into = luciole::mergeVisits(into, from); }

  std::int64_t secondsNow() {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return std::clamp<std::int64_t>(seconds, 0, luciole::kMaxVisitTime);
  }

  void mergeFound(Visit& into, const Part& part, const std::string& uri) {
    if (const auto found = part.find(uri); found != part.end()) {
      merge(into, found->second);
    }
  }

  // The location itself and every URI below it.
  void collectUnder(Part& into, const Part& part, const std::string& uri) {
    const auto children = uri.ends_with('/') ? uri : uri + '/';
    if (!uri.ends_with('/')) {
      if (const auto found = part.find(uri); found != part.end()) {
        merge(into[uri], found->second);
      }
    }
    for (auto item = part.lower_bound(children); item != part.end() && item->first.starts_with(children); ++item) {
      merge(into[item->first], item->second);
    }
  }
} // namespace

struct luciole::VisitService::State : std::enable_shared_from_this<State> {
  enum class Job : std::uint8_t { Load, Add, Clear };
  struct Result {
    std::vector<std::pair<std::size_t, Part>> parts;
    ConfigIssues issues;
  };

  State(std::filesystem::path directory, std::chrono::milliseconds wait) : store(std::move(directory)), delay(wait) {}
  VisitStore store; // Accessed only by serial worker tasks.
  std::chrono::milliseconds delay;
  VisitStore::Parts saved;       // as last read or written
  Part writing;                  // visits the running write carries
  Part pending;                  // visits not handed to a write yet
  std::int64_t pendingSince = 0; // monotonic microseconds of the first pending visit
  std::uint64_t generation = 0;  // advanced by clear, so results started before it are ignored
  GSource* timer = nullptr;
  GSource* notice = nullptr; // announces recorded visits
  bool active = false;
  bool loadNeeded = true;
  bool clearNeeded = false;
  bool flushNeeded = false;
  bool enabled = true;
  bool ready = false;
  std::string warning;
  std::function<void()> changed;

  void cancelTimer() { destroySource(timer); }

  // Notifies kNoticeMs from now unless a notice is already due.
  void announce() {
    if (notice != nullptr) {
      return;
    }
    notice = g_timeout_source_new(kNoticeMs);
    g_source_set_callback(
        notice,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->notice, nullptr));
          owner->notify();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(notice, g_main_context_default());
  }

  // Waits for a pause of `delay`, but no longer than four delays after the first pending visit.
  void schedule() {
    const auto now = g_get_monotonic_time();
    const auto delayMicros = static_cast<std::int64_t>(delay.count()) * 1000;
    const auto remaining = std::clamp<std::int64_t>(pendingSince + (4 * delayMicros) - now, 0, delayMicros);
    cancelTimer();
    timer = g_timeout_source_new(static_cast<guint>(remaining / 1000));
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->timer, nullptr));
          owner->flushNeeded = true;
          owner->pump();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(timer, g_main_context_default());
  }

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }

  void pump() {
    if (active) {
      return;
    }
    Job job = Job::Load;
    if (clearNeeded) {
      clearNeeded = false;
      job = Job::Clear;
    } else if (loadNeeded) {
      loadNeeded = false;
    } else if (flushNeeded && !pending.empty()) {
      flushNeeded = false;
      cancelTimer();
      job = Job::Add;
      writing = std::exchange(pending, {});
    } else {
      flushNeeded = false;
      return;
    }
    active = true;
    std::vector<VisitRecord> records;
    records.reserve(writing.size());
    for (const auto& [uri, visit] : writing) {
      records.push_back({uri, visit});
    }
    runJob<Result>(
        [owner = shared_from_this(), job, records = std::move(records)] {
          Result result;
          auto& store = owner->store;
          if (job == Job::Load) {
            store.load();
          } else if (job == Job::Clear) {
            (void)store.clear();
          } else {
            (void)store.add(records);
          }
          for (std::size_t part = 0; part < VisitStore::partCount; ++part) {
            const bool touched = job != Job::Add || std::ranges::any_of(records, [&](const VisitRecord& record) {
                                   return VisitStore::partOf(record.uri) == part;
                                 });
            if (touched) {
              result.parts.emplace_back(part, store.parts()[part]);
            }
          }
          result.issues = store.issues();
          return result;
        },
        [owner = shared_from_this(), job, generation = generation](JobResult<Result> result) {
          owner->finish(job, generation, std::move(result));
        }
    );
  }

  void finish(Job job, std::uint64_t started, JobResult<Result> result) {
    active = false;
    if (job == Job::Load) {
      ready = true;
    }
    if (job == Job::Add) {
      writing.clear();
    }
    if (!result) {
      warning = result.error().message;
    } else {
      for (const auto& issue : result->issues) {
        std::println(stderr, "luciole: warning: {}", issue.format());
      }
      warning = result->issues.empty() ? std::string{} : result->issues.front().format();
      if (started == generation) {
        for (auto& [part, visits] : result->parts) {
          saved[part] = std::move(visits);
        }
      }
    }
    pump();
    notify();
  }
};

luciole::VisitService::VisitService(std::filesystem::path directory, std::chrono::milliseconds delay)
    : m_state(std::make_shared<State>(std::move(directory), delay)) {
  m_state->pump();
}

luciole::VisitService::~VisitService() {
  m_state->changed = {};
  m_state->cancelTimer();
  destroySource(m_state->notice);
  // Unsaved visits are still written; the worker keeps the state alive until it finishes.
  m_state->flushNeeded = true;
  m_state->pump();
}

void luciole::VisitService::setEnabled(bool enabled) {
  if (m_state->enabled == enabled) {
    return;
  }
  m_state->enabled = enabled;
  if (!enabled) {
    m_state->pending.clear();
    m_state->flushNeeded = false;
    m_state->cancelTimer();
  }
  m_state->notify();
}

bool luciole::VisitService::enabled() const { return m_state->enabled; }

void luciole::VisitService::record(const Location& location) {
  if (!m_state->enabled || visitUriProblem(location.uri())) {
    return;
  }
  if (m_state->pending.empty()) {
    m_state->pendingSince = g_get_monotonic_time();
  }
  merge(m_state->pending[location.uri()], {.count = 1, .last = secondsNow()});
  if (m_state->pending.size() >= kPendingLimit) {
    flush();
  } else {
    m_state->schedule();
  }
  m_state->announce();
}

luciole::Visit luciole::VisitService::visit(const Location& location) const {
  Visit result;
  if (!m_state->enabled) {
    return result;
  }
  const auto& uri = location.uri();
  mergeFound(result, m_state->saved[VisitStore::partOf(uri)], uri);
  mergeFound(result, m_state->writing, uri);
  mergeFound(result, m_state->pending, uri);
  return result;
}

std::vector<luciole::VisitRecord> luciole::VisitService::visitsUnder(const Location& location) const {
  if (!m_state->enabled) {
    return {};
  }
  Part found;
  for (const auto& part : m_state->saved) {
    collectUnder(found, part, location.uri());
  }
  collectUnder(found, m_state->writing, location.uri());
  collectUnder(found, m_state->pending, location.uri());
  std::vector<VisitRecord> result;
  result.reserve(found.size());
  for (auto& [uri, visit] : found) {
    result.push_back({uri, visit});
  }
  return result;
}

void luciole::VisitService::clear() {
  auto& state = *m_state;
  ++state.generation;
  state.saved = {};
  state.writing.clear();
  state.pending.clear();
  state.flushNeeded = false;
  state.cancelTimer();
  state.loadNeeded = false;
  state.clearNeeded = true;
  state.pump();
  state.notify();
}

void luciole::VisitService::flush() {
  m_state->flushNeeded = true;
  m_state->pump();
}

bool luciole::VisitService::ready() const { return m_state->ready; }

bool luciole::VisitService::busy() const {
  return m_state->active || m_state->loadNeeded || m_state->clearNeeded || !m_state->pending.empty();
}

const std::string& luciole::VisitService::warning() const { return m_state->warning; }

void luciole::VisitService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
