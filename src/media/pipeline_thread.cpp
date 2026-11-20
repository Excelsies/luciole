#include "media/pipeline_thread.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <glib.h>
#include <memory>
#include <mutex>
#include <thread>

namespace {
  // How long process exit waits for the queued jobs to finish.
  constexpr std::chrono::seconds kExitWait{1};

  // Shared by the worker and its thread, so a thread left behind at exit still has it.
  struct Queue {
    std::mutex lock;
    std::condition_variable wake;
    std::condition_variable finished;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    bool done = false;
  };

  // Runs until the process ends and the queue is empty; each pass runs one job.
  void run(const std::shared_ptr<Queue>& queue) {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock lock(queue->lock);
        queue->wake.wait(lock, [&queue] { return queue->stopping || !queue->jobs.empty(); });
        if (queue->jobs.empty()) {
          queue->done = true;
          queue->finished.notify_all();
          return;
        }
        job = std::move(queue->jobs.front());
        queue->jobs.pop_front();
      }
      job();
    }
  }

  class Worker {
  public:
    Worker() : m_queue(std::make_shared<Queue>()), m_thread([queue = m_queue] { run(queue); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() {
      bool done = false;
      {
        std::unique_lock lock(m_queue->lock);
        m_queue->stopping = true;
        m_queue->wake.notify_one();
        done = m_queue->finished.wait_for(lock, kExitWait, [this] { return m_queue->done; });
      }
      if (done) {
        m_thread.join();
      } else {
        m_thread.detach();
      }
    }

    void post(std::function<void()> job) {
      {
        const std::scoped_lock lock(m_queue->lock);
        m_queue->jobs.push_back(std::move(job));
      }
      m_queue->wake.notify_one();
    }

  private:
    std::shared_ptr<Queue> m_queue;
    std::thread m_thread; // after the queue, so it starts once the queue exists
  };
} // namespace

void luciole::media::runOnPipelineThread(std::function<void()> job) {
  static Worker worker;
  worker.post(std::move(job));
}

void luciole::media::runOnProbeThread(std::function<void()> job) {
  static Worker worker;
  worker.post(std::move(job));
}

void luciole::media::runOnMainContext(std::function<void()> job) {
  GSource* source = g_idle_source_new();
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_callback(
      source,
      [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      new std::function<void()>(std::move(job)), [](gpointer data) { delete static_cast<std::function<void()>*>(data); }
  );
  (void)g_source_attach(source, g_main_context_default());
  g_source_unref(source);
}
