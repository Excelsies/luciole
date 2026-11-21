#pragma once

#include <string>

namespace luciole::media::bink2 {
  // The factory name of the demuxer.
  inline constexpr const char* kDemuxName = "luciolebink2demux";

  // Makes GStreamer know Bink 2 files: a typefinder for their signature and a demuxer that decodes them, both inside
  // this process. Starts GStreamer's registry work once; later calls return the first call's result. Empty on
  // success, else why it failed. Any thread, after GStreamer started.
  const std::string& registerElements();
} // namespace luciole::media::bink2
