#pragma once

#include "media/media_player.h"

#include <cstdint>
#include <gst/gst.h>
#include <memory>
#include <optional>
#include <span>

namespace luciole::media {
  // A few seconds of a pipeline's sound as mono samples. The streaming thread that carries the sound writes it
  // through a pad probe; the main context copies from it. Each sample keeps the stream time it is heard at, so a copy
  // takes the window that ends where the sink plays, however far ahead of the sink the sound arrives.
  struct AudioTap;

  // A tap of `rate` samples a second whose copies hold `length` samples; the caller checked both.
  std::shared_ptr<AudioTap> makeAudioTap(int rate, int length);
  // Watches the buffers and events that pass `pad`; the probe's id, or 0. The probe keeps the tap alive until it is
  // removed. Pipeline thread.
  gulong attachAudioTap(GstPad* pad, const std::shared_ptr<AudioTap>& tap);
  // Copies the window that ends at `heard`, a stream time in nanoseconds, or where the last copy ended when it is
  // not known. Main context.
  AudioWindow copyAudioWindow(AudioTap& tap, std::optional<std::int64_t> heard, std::span<float> samples);
} // namespace luciole::media
