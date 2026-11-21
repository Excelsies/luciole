#pragma once

#include <gst/gst.h>

namespace luciole::media {
  // How many threads a software video decoder needs for a stream: one per kPixelsPerThread pixels a second, between
  // one and the cores (at most kMaxDecoderThreads). 0 when the size is unknown, which leaves the decoder's own choice.
  // A missing frame rate counts as 60.
  int decoderThreads(int width, int height, int rateNumerator, int rateDenominator, int cores);

  // Gives a software video decoder the thread count its stream needs, when its caps arrive; any other element is
  // left alone.
  void limitDecoderThreads(GstElement* element);
} // namespace luciole::media
