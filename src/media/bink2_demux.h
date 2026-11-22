#pragma once

#include <string>

namespace luciole::media::bink2 {
  // The factory name of the demuxer.
  inline constexpr const char* kDemuxName = "luciolebink2demux";
  // The most pictures out at once, which is how far the element runs ahead of the clock; a picture is 12 MB at 4K.
  // The reference for the next picture is one of those already sent on, so a reader that keeps every picture holds all
  // of them.
  inline constexpr int kPoolMaximum = 5;

  // Makes GStreamer know Bink 2 files: a typefinder for their signature and a demuxer that decodes them, both inside
  // this process. Starts GStreamer's registry work once; later calls return the first call's result. Empty on
  // success, else why it failed. Any thread, after GStreamer started.
  const std::string& registerElements();
} // namespace luciole::media::bink2
