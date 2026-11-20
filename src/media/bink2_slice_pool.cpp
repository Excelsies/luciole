#include "media/bink2_slice_pool.h"

#include <cassert>

luciole::media::bink2::SlicePool::SlicePool() {
  for (int worker = 0; worker < kWorkers; ++worker) {
    m_threads[static_cast<std::size_t>(worker)] =
        std::jthread([this, worker](const std::stop_token& stop) { work(stop, worker); });
  }
}

luciole::media::bink2::SlicePool::~SlicePool() {
  for (auto& thread : m_threads) {
    thread.request_stop();
  }
  // The jthreads join as the array is destroyed, after this body.
}

void luciole::media::bink2::SlicePool::runJobs(int count, void* context, Call call) {
  assert(count >= 0 && count <= kMaxJobs);
  {
    const std::scoped_lock lock(m_lock);
    m_context = context;
    m_call = call;
    m_count = count;
    m_finished = 0;
    m_next = 0;
    ++m_generation;
  }
  m_wake.notify_all();
  std::unique_lock lock(m_lock);
  m_done.wait(lock, [this] { return m_finished == kWorkers; });
}

// Ends when the destructor asks the thread to stop. Each pass takes one run: it waits for a new generation, takes
// indexes below the count, and reports in; the caller waits for every worker, so none lags into the next run.
void luciole::media::bink2::SlicePool::work(const std::stop_token& stop, int worker) {
  unsigned seen = 0;
  std::unique_lock lock(m_lock);
  while (!stop.stop_requested()) {
    if (!m_wake.wait(lock, stop, [&] { return m_generation != seen; })) {
      return;
    }
    seen = m_generation;
    void* const context = m_context;
    const Call call = m_call;
    const int count = m_count;
    lock.unlock();
    for (int index = m_next.fetch_add(1); index < count; index = m_next.fetch_add(1)) {
      call(context, worker, index);
    }
    lock.lock();
    if (++m_finished == kWorkers) {
      m_done.notify_one();
    }
  }
}
