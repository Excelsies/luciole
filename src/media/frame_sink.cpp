#include "media/gstreamer_pipeline.h"
#include "media/pipeline_thread.h"

#include <algorithm>
#include <cmath>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <span>

namespace {
  using luciole::media::Pipeline;

  std::pair<int, int> fitted(int width, int height, int parN, int parD, int boxWidth, int boxHeight) {
    if (width <= 0 || height <= 0 || parN <= 0 || parD <= 0) {
      return {0, 0};
    }
    constexpr int kMax = luciole::kMaxMediaFrameSide;
    const int boxW = boxWidth > 0 ? std::min(boxWidth, kMax) : kMax;
    const int boxH = boxHeight > 0 ? std::min(boxHeight, kMax) : kMax;
    const double shown = static_cast<double>(width) * parN / parD;
    const double scale = std::min({1.0, boxW / shown, boxH / static_cast<double>(height)});
    return {
        std::clamp(static_cast<int>(std::lround(shown * scale)), 1, boxW),
        std::clamp(static_cast<int>(std::lround(height * scale)), 1, boxH),
    };
  }

  std::shared_ptr<Pipeline> locked(gpointer data) { return static_cast<std::weak_ptr<Pipeline>*>(data)->lock(); }
  void forget(gpointer data) { delete static_cast<std::weak_ptr<Pipeline>*>(data); }

  // Streaming thread: the size and aspect of the decoded pictures.
  GstPadProbeReturn sourceCaps(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer data) {
    GstEvent* event = gst_pad_probe_info_get_event(info);
    const auto pipeline = locked(data);
    if (event != nullptr && pipeline && GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      pipeline->stats.lastTime = -1; // a seek or a new segment: the next picture is not one after the last
    }
    if (event == nullptr || GST_EVENT_TYPE(event) != GST_EVENT_CAPS || !pipeline) {
      return GST_PAD_PROBE_OK;
    }
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    GstVideoInfo video;
    if (caps == nullptr || gst_video_info_from_caps(&video, caps) == FALSE) {
      return GST_PAD_PROBE_OK;
    }
    {
      const std::scoped_lock lock(pipeline->lock);
      pipeline->sourceWidth = GST_VIDEO_INFO_WIDTH(&video);
      pipeline->sourceHeight = GST_VIDEO_INFO_HEIGHT(&video);
      pipeline->parN = GST_VIDEO_INFO_PAR_N(&video);
      pipeline->parD = GST_VIDEO_INFO_PAR_D(&video);
    }
    pipeline->video = true;
    if (GST_VIDEO_INFO_FPS_N(&video) > 0) {
      pipeline->stats.frameLength = gst_util_uint64_scale(
          GST_SECOND, static_cast<guint64>(GST_VIDEO_INFO_FPS_D(&video)), static_cast<guint64>(GST_VIDEO_INFO_FPS_N(&video))
      );
    }
    (void)luciole::media::fitFrames(*pipeline);
    return GST_PAD_PROBE_OK;
  }

  // The stream time of a frame in nanoseconds, or -1 when it has none.
  std::int64_t streamTime(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    const GstSegment* segment = gst_sample_get_segment(sample);
    if (buffer == nullptr || segment == nullptr || !GST_BUFFER_PTS_IS_VALID(buffer)) {
      return -1;
    }
    const GstClockTime time = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    return GST_CLOCK_TIME_IS_VALID(time) ? static_cast<std::int64_t>(time) : -1;
  }

  // Streaming thread: keeps only the newest frame and tells the main context once until it is taken.
  GstFlowReturn keep(GstSample* sample, gpointer data) {
    const auto pipeline = locked(data);
    if (sample == nullptr || !pipeline) {
      if (sample != nullptr) {
        gst_sample_unref(sample);
      }
      return GST_FLOW_OK;
    }
    GstSample* old = nullptr;
    const std::int64_t time = streamTime(sample);
    pipeline->frameTime = time;
    // The preroll picture comes again as the first sample: the same time, and not a new picture.
    const bool again = time >= 0 && time == pipeline->stats.lastTime;
    if (!again) {
      pipeline->stats.delivered++;
      if (const auto gap = luciole::media::missedFrames(pipeline->stats.lastTime, time, pipeline->stats.frameLength);
          gap > 0) {
        pipeline->stats.missed += gap;
      }
      pipeline->stats.lastTime = time;
    }
    {
      const std::scoped_lock lock(pipeline->lock);
      old = std::exchange(pipeline->latest, sample);
    }
    if (old != nullptr) {
      if (!again) {
        pipeline->stats.replaced++;
      }
      gst_sample_unref(old);
    }
    if (!pipeline->framePosted.exchange(true)) {
      luciole::media::runOnMainContext([pipeline] {
        pipeline->framePosted = false;
        if (const auto heard = pipeline->listener.frame) {
          heard();
        }
      });
    }
    return GST_FLOW_OK;
  }

  // Owns a sample and its mapping for as long as a frame's pixels are used.
  struct MappedSample {
    GstSample* sample = nullptr;
    GstVideoFrame frame{};
    bool mapped = false;

    explicit MappedSample(GstSample* owned) : sample(owned) {}
    MappedSample(const MappedSample&) = delete;
    MappedSample& operator=(const MappedSample&) = delete;
    ~MappedSample() {
      if (mapped) {
        gst_video_frame_unmap(&frame);
      }
      gst_sample_unref(sample);
    }
  };
} // namespace

bool luciole::media::fitFrames(Pipeline& pipeline) {
  const std::scoped_lock fitting(pipeline.fitLock);
  GstElement* filter = nullptr;
  std::pair<int, int> size;
  {
    const std::scoped_lock lock(pipeline.lock);
    size = fitted(
        pipeline.sourceWidth, pipeline.sourceHeight, pipeline.parN, pipeline.parD, pipeline.boxWidth, pipeline.boxHeight
    );
    if (pipeline.filter == nullptr || size.first == 0 || size == pipeline.applied) {
      return false;
    }
    pipeline.applied = size;
    filter = GST_ELEMENT(gst_object_ref(pipeline.filter));
  }
  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT, size.first, "height", G_TYPE_INT,
      size.second, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr
  );
  g_object_set(filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  gst_object_unref(filter);
  return true;
}

GstElement* luciole::media::frameSink(const std::shared_ptr<Pipeline>& pipeline, GstElement*& filter) {
  GstElement* bin = gst_bin_new(nullptr);
  GstElement* convert = gst_element_factory_make("videoconvertscale", nullptr);
  filter = gst_element_factory_make("capsfilter", nullptr);
  GstElement* sink = gst_element_factory_make("appsink", nullptr);
  if (bin == nullptr || convert == nullptr || filter == nullptr || sink == nullptr) {
    for (GstElement* element : {bin, convert, filter, sink}) {
      if (element != nullptr) {
        gst_object_unref(gst_object_ref_sink(element));
      }
    }
    filter = nullptr;
    return nullptr;
  }
  gst_bin_add_many(GST_BIN(bin), convert, filter, sink, nullptr);
  GstPad* pad = gst_element_get_static_pad(convert, "sink");
  if (gst_element_link_many(convert, filter, sink, nullptr) == FALSE
      || pad == nullptr
      || gst_element_add_pad(bin, gst_ghost_pad_new("sink", pad)) == FALSE) {
    if (pad != nullptr) {
      gst_object_unref(pad);
    }
    gst_object_unref(gst_object_ref_sink(bin));
    filter = nullptr;
    return nullptr;
  }
  (void)gst_pad_add_probe(
      pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, sourceCaps, new std::weak_ptr<Pipeline>(pipeline), forget
  );
  gst_object_unref(pad);
  GstCaps* rgba = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, "RGBA", "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr
  );
  g_object_set(filter, "caps", rgba, nullptr);
  // One frame waits in the sink and one in `latest`; the sink keeps no extra last frame of its own.
  g_object_set(sink, "caps", rgba, "max-buffers", 1U, "drop", TRUE, "sync", TRUE, "enable-last-sample", FALSE, nullptr);
  gst_caps_unref(rgba);
  GstAppSinkCallbacks callbacks{};
  callbacks.new_preroll = [](GstAppSink* appsink, gpointer data) {
    return keep(gst_app_sink_pull_preroll(appsink), data);
  };
  callbacks.new_sample = [](GstAppSink* appsink, gpointer data) {
    return keep(gst_app_sink_pull_sample(appsink), data);
  };
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, new std::weak_ptr<Pipeline>(pipeline), forget);
  return bin;
}

std::optional<luciole::MediaFrame> luciole::media::takeFrame(Pipeline& pipeline) {
  GstSample* sample = nullptr;
  {
    const std::scoped_lock lock(pipeline.lock);
    sample = std::exchange(pipeline.latest, nullptr);
  }
  if (sample == nullptr) {
    return std::nullopt;
  }
  pipeline.stats.taken++;
  auto held = std::make_shared<MappedSample>(sample);
  GstVideoInfo info;
  GstCaps* caps = gst_sample_get_caps(sample);
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (caps == nullptr
      || buffer == nullptr
      || gst_video_info_from_caps(&info, caps) == FALSE
      || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA) {
    return std::nullopt;
  }
  held->mapped = gst_video_frame_map(&held->frame, &info, buffer, GST_MAP_READ) != FALSE;
  if (!held->mapped) {
    return std::nullopt;
  }
  const int width = GST_VIDEO_FRAME_WIDTH(&held->frame);
  const int height = GST_VIDEO_FRAME_HEIGHT(&held->frame);
  if (width < 1
      || height < 1
      || width > kMaxMediaFrameSide
      || height > kMaxMediaFrameSide
      || GST_VIDEO_FRAME_PLANE_STRIDE(&held->frame, 0) != width * 4) {
    return std::nullopt;
  }
  const auto* pixels = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&held->frame, 0));
  const std::span rgba(pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
  const std::int64_t time = streamTime(sample);
  return MediaFrame{.width = width,
                    .height = height,
                    .rgba = rgba,
                    .hold = std::move(held),
                    .time = time >= 0 ? static_cast<double>(time) / static_cast<double>(GST_SECOND) : -1};
}
