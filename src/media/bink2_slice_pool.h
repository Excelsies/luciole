#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace luciole::media::bink2 {
  // A fixed set of worker threads that run the slices of one frame at the same time. The threads start with the
  // pool and are joined when it ends; none is detached.
  class SlicePool {
  public:
    static constexpr int kWorkers = 4;
    // The most jobs one run takes: the slices of a frame.
    static constexpr int kMaxJobs = 8;

    SlicePool();
    ~SlicePool();

    SlicePool(const SlicePool&) = delete;
    SlicePool& operator=(const SlicePool&) = delete;

    // Calls task(worker, index) for each index below `count` (at most kMaxJobs) on the workers and returns when all
    // are done. `worker` is the thread's own number below kWorkers, for state a thread keeps. One caller at a time;
    // `task` must not throw.
    template <typename Task> void run(int count, Task&& task) {
      runJobs(count, &task, [](void* context, int worker, int index) {
        (*static_cast<Task*>(context))(worker, index);
      });
    }

  private:
    using Call = void (*)(void* context, int worker, int index);
    void runJobs(int count, void* context, Call call);
    void work(const std::stop_token& stop, int worker);

    std::mutex m_lock;
    std::condition_variable_any m_wake;
    std::condition_variable m_done;
    // Guarded by m_lock.
    void* m_context = nullptr;
    Call m_call = nullptr;
    int m_count = 0;
    unsigned m_generation = 0;
    int m_finished = 0;
    std::atomic<int> m_next{0};
    std::array<std::jthread, kWorkers> m_threads;
  };
} // namespace luciole::media::bink2
