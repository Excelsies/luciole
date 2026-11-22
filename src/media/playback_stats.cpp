#include "media/playback_stats.h"

#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <string_view>

std::uint64_t luciole::media::missedFrames(std::int64_t last, std::int64_t time, std::int64_t frameLength) {
  if (last < 0 || time <= last || frameLength <= 0) {
    return 0;
  }
  // Rounded to the nearest whole number of frame lengths.
  const std::int64_t frames = (time - last + frameLength / 2) / frameLength;
  return frames > 1 ? static_cast<std::uint64_t>(frames - 1) : 0;
}

void luciole::media::tracePlayback(const PlaybackStats& stats) {
  const char* value = std::getenv("LUCIOLE_STARTUP_TRACE");
  if (value == nullptr || std::string_view(value) != "1") {
    return;
  }
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const auto nanoseconds = (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U) + static_cast<std::uint64_t>(now.tv_nsec);
  // fprintf cannot throw, as println can, and this runs in a pipeline thread job.
  std::fprintf(
      stderr, "luciole-trace %llu playback delivered=%llu taken=%llu replaced=%llu missed=%llu\n",
      static_cast<unsigned long long>(nanoseconds), static_cast<unsigned long long>(stats.delivered.load()),
      static_cast<unsigned long long>(stats.taken.load()), static_cast<unsigned long long>(stats.replaced.load()),
      static_cast<unsigned long long>(stats.missed.load())
  );
}
