#pragma once

#include "media/media_player.h"

#include <gst/gst.h>
#include <string>
#include <string_view>

namespace luciole::media {
  inline constexpr std::string_view kCannotPlay = "The file cannot be played.";

  // Why GStreamer cannot play; empty once it runs and has every element playback needs. The first call starts
  // GStreamer and keeps decoders that need a graphics device out of automatic choice, so a preview decodes on the
  // processor. Any thread.
  const std::string& startProblem();
  // Why `path` cannot be read as a media file; empty for a regular file. Only its status is read, so a FIFO or a
  // device is refused without being opened.
  std::string fileProblem(const std::string& path);
  // Makes every element added below `bin` after this call that would reach the network fail, ending the bin with an
  // error; a parser or decoder bin below it never builds one.
  void refuseNetwork(GstElement* bin);

  // Where a pipeline sends its sound: an opened sink owned by the caller, or null when none could be made.
  struct [[nodiscard]] AudioSink {
    GstElement* element = nullptr;
    bool deviceMissing = false; // the system's output did not open, so `element` plays nothing
  };
  // The system's default output opened to READY, or a sink that keeps the clock and plays nothing when `output`
  // asks for silence or the system's output does not open.
  AudioSink openAudioSink(AudioOutput output);
} // namespace luciole::media
