#include "fs/subtree_measure.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <glib.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

struct luciole::detail::MeasureJob {
  explicit MeasureJob(Location root) : location(std::move(root)) {}
  Location location;
  CancelToken cancel;
  std::uint64_t sequence = 0;
  MeasurePriority priority = MeasurePriority::Current; // guarded by Queue::mutex
  // Written by the worker, taken by the main context.
  std::mutex mutex;
  std::shared_ptr<const MeasureTable> latest;
  MeasurePriority walked = MeasurePriority::Current;
  bool finished = false;
  // Main context only.
  std::shared_ptr<const MeasureTable> published;
  MeasurePriority publishedWalk = MeasurePriority::Current;
  bool done = false;
  std::size_t handles = 0;
};

namespace {
  using luciole::detail::MeasureJob;

  struct Queue {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<MeasureJob>> jobs;
    std::size_t running = 0;
    bool stopping = false;
  };

  luciole::MeasureTable failedTable(const char* message) {
    luciole::MeasureTable table;
    table.error = luciole::FileError{.kind = luciole::FileErrorKind::Other, .message = message};
    return table;
  }

  // The most urgent queued job: lowest priority value, then the earliest request. Caller holds the queue mutex.
  std::shared_ptr<MeasureJob> takeNext(Queue& queue) {
    const auto next = std::ranges::min_element(queue.jobs, [](const auto& a, const auto& b) {
      return a->priority != b->priority ? a->priority < b->priority : a->sequence < b->sequence;
    });
    auto job = std::move(*next);
    queue.jobs.erase(next);
    return job;
  }

  // Walks one job. The running count drops before the final table is stored, so a published result never
  // shows its walk as still running.
  void walkJob(
      MeasureJob& job, Queue& queue, const luciole::MeasureServiceOptions& service, luciole::MeasurePriority priority
  ) {
    auto options = service.walk;
    options.cancel = job.cancel;
    options.progressInterval = service.publishInterval;
    if (priority == luciole::MeasurePriority::Ancestor) {
      options.entryBudget = std::min(options.entryBudget, service.ancestorBudget);
      options.deadline = std::chrono::steady_clock::now() + service.ancestorTime;
    }
    const auto store = [&job, priority](luciole::MeasureTable table, bool finished) {
      auto shared = std::make_shared<const luciole::MeasureTable>(std::move(table));
      const std::scoped_lock lock(job.mutex);
      job.latest = std::move(shared);
      job.walked = priority;
      job.finished = finished;
    };
    luciole::MeasureTable result;
    try {
      result = luciole::measureSubtree(job.location, options, [&](luciole::MeasureTable table) {
        store(std::move(table), false);
        if (service.afterPartial) {
          service.afterPartial();
        }
      });
    } catch (const std::exception&) {
      result = failedTable("The folder could not be measured.");
    }
    {
      const std::scoped_lock lock(queue.mutex);
      --queue.running;
    }
    store(std::move(result), true);
  }

  // A worker thread: runs queued jobs one at a time until the service stops.
  void work(const std::shared_ptr<Queue>& queue, const luciole::MeasureServiceOptions& options) {
    ::pthread_setname_np(::pthread_self(), "luciole-measure");
    for (;;) {
      std::shared_ptr<MeasureJob> job;
      luciole::MeasurePriority priority{};
      {
        std::unique_lock lock(queue->mutex);
        queue->wake.wait(lock, [&] { return queue->stopping || !queue->jobs.empty(); });
        if (queue->stopping) {
          return;
        }
        job = takeNext(*queue);
        priority = job->priority;
        ++queue->running;
      }
      walkJob(*job, *queue, options, priority);
    }
  }
} // namespace

struct luciole::detail::MeasureServiceState : std::enable_shared_from_this<MeasureServiceState> {
  MeasureServiceOptions options;
  std::shared_ptr<Queue> queue = std::make_shared<Queue>();
  std::vector<std::thread> threads;
  std::unordered_map<std::string, std::shared_ptr<MeasureJob>> pending; // queued or running, by URI
  std::function<void()> changed;
  GSource* timer = nullptr;
  std::uint64_t sequence = 0;

  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }

  void stopTimer() {
    if (timer != nullptr) {
      g_source_destroy(timer);
      g_source_unref(std::exchange(timer, nullptr));
    }
  }

  // Publishes what workers stored since the last poll; bounded by the pending jobs.
  void poll() {
    bool any = false;
    for (auto it = pending.begin(); it != pending.end();) {
      auto& job = *it->second;
      std::shared_ptr<const MeasureTable> latest;
      MeasurePriority walked{};
      bool finished = false;
      {
        const std::scoped_lock lock(job.mutex);
        latest = std::move(job.latest);
        walked = job.walked;
        finished = job.finished;
      }
      if (latest) {
        job.published = std::move(latest);
        job.publishedWalk = walked;
        any = true;
      }
      if (finished) {
        job.done = true;
        it = pending.erase(it);
      } else {
        ++it;
      }
    }
    if (pending.empty()) {
      stopTimer();
    }
    if (any) {
      notify();
    }
  }

  void startTimer() {
    if (timer != nullptr) {
      return;
    }
    timer = g_timeout_source_new(static_cast<guint>(std::max<std::int64_t>(1, options.publishInterval.count())));
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          const auto state = static_cast<std::weak_ptr<MeasureServiceState>*>(data)->lock();
          if (!state || state->timer == nullptr) {
            return G_SOURCE_REMOVE;
          }
          state->poll();
          return state->timer != nullptr ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        },
        new std::weak_ptr<MeasureServiceState>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<MeasureServiceState>*>(data); }
    );
    g_source_attach(timer, g_main_context_default());
  }

  void startWorkers() {
    const auto count = std::clamp<std::size_t>(options.workers, 1, 2);
    while (threads.size() < count) {
      threads.emplace_back([queue = queue, &options = options] { work(queue, options); });
    }
  }

  // Drops one handle; the last one of an unfinished job withdraws it from the queue or cancels its walk.
  void release(const std::shared_ptr<MeasureJob>& job) {
    if (--job->handles > 0 || job->done) {
      return;
    }
    job->cancel.cancel();
    {
      const std::scoped_lock lock(queue->mutex);
      std::erase(queue->jobs, job);
    }
    if (const auto found = pending.find(job->location.uri()); found != pending.end() && found->second == job) {
      pending.erase(found);
    }
    if (pending.empty()) {
      stopTimer();
    }
  }

  void shutdown() {
    stopTimer();
    {
      const std::scoped_lock lock(queue->mutex);
      queue->stopping = true;
      queue->jobs.clear();
    }
    for (const auto& [uri, job] : pending) {
      job->cancel.cancel();
    }
    queue->wake.notify_all();
    for (auto& thread : threads) {
      thread.join();
    }
    threads.clear();
    pending.clear();
    changed = {};
  }
};

luciole::MeasureHandle::~MeasureHandle() { reset(); }

luciole::MeasureHandle::MeasureHandle(MeasureHandle&& other) noexcept
    : m_job(std::move(other.m_job)), m_service(std::move(other.m_service)) {}

luciole::MeasureHandle& luciole::MeasureHandle::operator=(MeasureHandle&& other) noexcept {
  if (this != &other) {
    reset();
    m_job = std::move(other.m_job);
    m_service = std::move(other.m_service);
  }
  return *this;
}

std::shared_ptr<const luciole::MeasureTable> luciole::MeasureHandle::table() const {
  return m_job ? m_job->published : nullptr;
}

bool luciole::MeasureHandle::done() const { return m_job && m_job->done; }

luciole::MeasurePriority luciole::MeasureHandle::walkPriority() const {
  return m_job ? m_job->publishedWalk : MeasurePriority::Current;
}

void luciole::MeasureHandle::reset() {
  if (!m_job) {
    return;
  }
  if (const auto service = m_service.lock()) {
    service->release(m_job);
  }
  m_job.reset();
  m_service.reset();
}

luciole::SubtreeMeasureService::SubtreeMeasureService(MeasureServiceOptions options)
    : m_state(std::make_shared<detail::MeasureServiceState>()) {
  m_state->options = std::move(options);
}

luciole::SubtreeMeasureService::~SubtreeMeasureService() { m_state->shutdown(); }

luciole::MeasureHandle luciole::SubtreeMeasureService::request(const Location& root, MeasurePriority priority) {
  auto& state = *m_state;
  auto& job = state.pending[root.uri()];
  if (job) {
    const std::scoped_lock lock(state.queue->mutex);
    job->priority = std::min(job->priority, priority);
  } else {
    job = std::make_shared<detail::MeasureJob>(root);
    job->sequence = ++state.sequence;
    job->priority = priority;
    {
      const std::scoped_lock lock(state.queue->mutex);
      state.queue->jobs.push_back(job);
    }
    state.queue->wake.notify_one();
    state.startWorkers();
    state.startTimer();
  }
  ++job->handles;
  MeasureHandle handle;
  handle.m_job = job;
  handle.m_service = m_state;
  return handle;
}

luciole::MeasureServiceStatus luciole::SubtreeMeasureService::status() const {
  const std::scoped_lock lock(m_state->queue->mutex);
  return {.queued = m_state->queue->jobs.size(), .running = m_state->queue->running};
}

void luciole::SubtreeMeasureService::setChangedCallback(std::function<void()> changed) {
  m_state->changed = std::move(changed);
}
