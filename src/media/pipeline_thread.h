#pragma once

#include <functional>

namespace luciole::media {
  // Runs `job` on the one pipeline thread, after the jobs posted before it. At process exit the thread finishes the
  // queued jobs for at most a second; a job still running then is left behind, so a blocked job never keeps the
  // process from ending.
  void runOnPipelineThread(std::function<void()> job);
  // The same for reading media facts, on a thread of its own, so a slow file never holds up playback.
  void runOnProbeThread(std::function<void()> job);
  // A job posted to the pipeline thread, such as the stop that closes a playing file, is queued or running. Any thread;
  // never starts the thread.
  bool pipelineThreadBusy() noexcept;
  // The same for the probe thread, which opens files to read their facts.
  bool probeThreadBusy() noexcept;
  // Runs `job` on the default main context at default priority, the bus watches' own, so a busy bus never holds it
  // back. Any thread.
  void runOnMainContext(std::function<void()> job);
} // namespace luciole::media
