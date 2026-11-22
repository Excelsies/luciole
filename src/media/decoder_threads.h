#pragma once

#include <gst/gst.h>

namespace luciole::media {
  // How many threads a software video decoder needs for a stream: one per 60 million pixels a second, between one and
  // the cores (at most 16). 0 when the size or the core count is unknown, which leaves the decoder's own choice.
  // A missing frame rate counts as 60.
  // A stream deeper than 8 bits per sample costs more to decode and counts at one and a half times its pixel rate.
  int decoderThreads(int width, int height, int rateNumerator, int rateDenominator, int cores, int bitDepth = 8);

  // Gives a software video decoder the thread count its stream needs, when its caps arrive; any other element is
  // left alone.
  void limitDecoderThreads(GstElement* element);
} // namespace luciole::media
