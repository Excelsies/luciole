#include "media/decoder_threads.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <thread>

namespace {
  // A software decoder runs at several hundred million pixels a second per thread even on hard streams (a 4K
  // stream at 64 Mbit/s decodes at 35 frames a second on one thread of this machine, 291 Mpixel/s); this counts one
  // thread for a fifth of that, so a stream always decodes with room to spare.
  constexpr std::int64_t kPixelsPerThread = 60'000'000;
  constexpr int kMaxDecoderThreads = 16;
  constexpr int kAssumedRate = 60;

  // The property that bounds a software decoder's threads, or null.
  GParamSpec* threadProperty(GstElement* element) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(element);
    for (const char* name : {"max-threads", "n-threads"}) {
      GParamSpec* spec = g_object_class_find_property(klass, name);
      if (spec != nullptr && (spec->value_type == G_TYPE_INT || spec->value_type == G_TYPE_UINT)) {
        return spec;
      }
    }
    return nullptr;
  }

  bool isVideoDecoder(GstElement* element) {
    GstElementFactory* factory = gst_element_get_factory(element);
    const char* klass = factory != nullptr ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : nullptr;
    const std::string_view text = klass != nullptr ? klass : "";
    return text.contains("Decoder") && text.contains("Video");
  }

  // Streaming thread, before the decoder sees the caps: sets the threads the stream needs.
  GstPadProbeReturn onCaps(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer data) {
    GstEvent* event = gst_pad_probe_info_get_event(info);
    if (event == nullptr || GST_EVENT_TYPE(event) != GST_EVENT_CAPS) {
      return GST_PAD_PROBE_OK;
    }
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    const GstStructure* shape = caps != nullptr && gst_caps_get_size(caps) > 0 ? gst_caps_get_structure(caps, 0) : nullptr;
    auto* element = static_cast<GstElement*>(data);
    GParamSpec* spec = threadProperty(element);
    int width = 0;
    int height = 0;
    int numerator = 0;
    int denominator = 1;
    if (shape == nullptr
        || spec == nullptr
        || !gst_structure_get_int(shape, "width", &width)
        || !gst_structure_get_int(shape, "height", &height)) {
      return GST_PAD_PROBE_OK;
    }
    (void)gst_structure_get_fraction(shape, "framerate", &numerator, &denominator);
    const int threads = luciole::media::decoderThreads(
        width, height, numerator, denominator, static_cast<int>(std::thread::hardware_concurrency())
    );
    if (threads > 0) {
      GValue value = G_VALUE_INIT;
      g_value_init(&value, spec->value_type);
      if (spec->value_type == G_TYPE_INT) {
        g_value_set_int(&value, threads);
      } else {
        g_value_set_uint(&value, static_cast<guint>(threads));
      }
      g_object_set_property(G_OBJECT(element), spec->name, &value);
      g_value_unset(&value);
    }
    return GST_PAD_PROBE_OK;
  }
} // namespace

int luciole::media::decoderThreads(int width, int height, int rateNumerator, int rateDenominator, int cores) {
  if (width <= 0 || height <= 0) {
    return 0;
  }
  const double rate = rateNumerator > 0 && rateDenominator > 0 ? static_cast<double>(rateNumerator) / rateDenominator
                                                                : kAssumedRate;
  const auto pixelsPerSecond = static_cast<std::int64_t>(static_cast<double>(width) * height * rate);
  const auto needed = static_cast<int>((pixelsPerSecond + kPixelsPerThread - 1) / kPixelsPerThread);
  return std::clamp(needed, 1, std::clamp(cores, 1, kMaxDecoderThreads));
}

void luciole::media::limitDecoderThreads(GstElement* element) {
  if (!isVideoDecoder(element) || threadProperty(element) == nullptr) {
    return;
  }
  GstPad* pad = gst_element_get_static_pad(element, "sink");
  if (pad == nullptr) {
    return;
  }
  (void)gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, onCaps, element, nullptr);
  gst_object_unref(pad);
}
