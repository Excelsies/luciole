#pragma once

#include <atomic>
#include <cstdint>

namespace luciole::media {
  // What one pipeline delivered to its frame sink, for benchmarks: printed once when the pipeline is freed, and only
  // when LUCIOLE_STARTUP_TRACE=1.
  struct PlaybackStats {
    std::atomic<std::uint64_t> delivered{0};     // different pictures the sink received (the preroll one counts once)
    std::atomic<std::uint64_t> taken{0};         // different pictures the view took to draw
    std::atomic<std::uint64_t> replaced{0};      // pictures a newer one replaced before the view took them
    std::atomic<std::uint64_t> missed{0};        // pictures the stream's timing says never arrived
    std::atomic<std::int64_t> lastTakenTime{-1}; // the stream time of the last picture the view took
    // Streaming thread only: the stream time of the last picture and the length of one, in nanoseconds; -1 and 0 when
    // unknown.
    std::int64_t lastTime = -1;
    std::int64_t frameLength = 0;
  };

  // How many pictures lie between two consecutive ones of a steady stream: the gap in frame lengths, less the one
  // the second picture itself is. 0 for an unknown time or length, a time that goes back, or a gap within half a
  // frame of one.
  std::uint64_t missedFrames(std::int64_t last, std::int64_t time, std::int64_t frameLength);

  // Prints the counts as a "playback" line of the startup trace.
  void tracePlayback(const PlaybackStats& stats);
} // namespace luciole::media
