#include "media/pipeline_thread.h"
#include "test_check.h"

#include <array>
#include <future>
#include <print>
#include <thread>
#include <unistd.h>
#include <vector>

// Jobs run in order on one thread other than the caller's, and a job blocked for good does not keep the process
// from ending: this suite passes only if it exits.
int main() {
  std::vector<int> order;
  std::vector<std::thread::id> threads;
  std::promise<void> ran;
  for (int i = 1; i <= 3; ++i) {
    luciole::media::runOnPipelineThread([&order, &threads, i] {
      order.push_back(i);
      threads.push_back(std::this_thread::get_id());
    });
  }
  luciole::media::runOnPipelineThread([&ran] { ran.set_value(); });
  ran.get_future().get();
  TEST_CHECK(order.size() == 3);
  TEST_CHECK(order[0] == 1 && order[1] == 2 && order[2] == 3);
  TEST_CHECK(threads[0] != std::this_thread::get_id());
  TEST_CHECK(threads[0] == threads[1] && threads[1] == threads[2]);

  // A read from a pipe nobody writes to never returns.
  std::array<int, 2> pipe{-1, -1};
  TEST_CHECK(::pipe(pipe.data()) == 0);
  std::promise<void> blocked;
  luciole::media::runOnPipelineThread([&blocked, readEnd = pipe[0]] {
    blocked.set_value();
    char byte = 0;
    (void)::read(readEnd, &byte, 1);
  });
  blocked.get_future().get();
  std::println("media-pipeline-thread: returning with a blocked job");
  return 0;
}
