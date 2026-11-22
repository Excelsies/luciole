// The playback mode of the Bink 2 bench: plays a file once through playbin as the player does.
#include "bink2_bench_common.h"
#include "media/gstreamer_runtime.h"

#include <algorithm>
#include <format>
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <string>

namespace bink2bench {
  namespace {
    struct Playing {
      GMainLoop* loop = nullptr;
      int received = 0;
      int dropped = 0;
      int processed = 0;
      bool failed = false;
      // Each picture's arrival against its time: the first picture sets the zero, and one that comes more than a frame
      // period after its time is late.
      bool started = false;
      Clock::time_point zero;
      GstClockTime firstTime = 0;
      int late = 0;
      double worstLate = 0;
    };

    gboolean onBus(GstBus* /*bus*/, GstMessage* message, gpointer data) {
      auto& playing = *static_cast<Playing*>(data);
      switch (GST_MESSAGE_TYPE(message)) {
      case GST_MESSAGE_QOS: {
        guint64 processed = 0;
        guint64 dropped = 0;
        gst_message_parse_qos_stats(message, nullptr, &processed, &dropped);
        playing.processed = static_cast<int>(processed);
        playing.dropped = static_cast<int>(dropped);
        break;
      }
      case GST_MESSAGE_ERROR: {
        GError* error = nullptr;
        gst_message_parse_error(message, &error, nullptr);
        std::println(stderr, "error: {}", error != nullptr ? error->message : "");
        g_clear_error(&error);
        playing.failed = true;
        g_main_loop_quit(playing.loop);
        break;
      }
      case GST_MESSAGE_EOS:
        g_main_loop_quit(playing.loop);
        break;
      default:
        break;
      }
      return G_SOURCE_CONTINUE;
    }

  } // namespace

  int play(const char* path, int width, int height) {
    if (const auto& problem = luciole::media::startProblem(); !problem.empty()) {
      std::println(stderr, "{}", problem);
      return 2;
    }
    const File probe(path);
    const auto stream = open(probe);
    const double seconds = static_cast<double>(stream->info().frames)
        * stream->info().frameRateDenominator
        / stream->info().frameRateNumerator;
    Playing playing;
    playing.loop = g_main_loop_new(nullptr, FALSE);
    const std::string description = std::format(
        "videoconvertscale ! video/x-raw,format=RGBA,width={},height={} ! appsink name=sink sync=true drop=true "
        "max-buffers=1 enable-last-sample=false",
        width, height
    );
    GError* error = nullptr;
    GstElement* videoSink = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    // The sink counts and drops each picture as it arrives, as the player's does.
    GstElement* sink = gst_bin_get_by_name(GST_BIN(videoSink), "sink");
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = [](GstAppSink* appsink, gpointer data) {
      GstSample* sample = gst_app_sink_pull_sample(appsink);
      if (sample != nullptr) {
        auto& playing = *static_cast<Playing*>(data);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        ++playing.received;
        if (!playing.started) {
          playing.started = true;
          playing.zero = Clock::now();
          playing.firstTime = GST_BUFFER_PTS(buffer);
        }
        const double due = static_cast<double>(GST_BUFFER_PTS(buffer) - playing.firstTime) / GST_MSECOND;
        const double late = milliseconds(playing.zero) - due;
        const double period = static_cast<double>(GST_BUFFER_DURATION(buffer)) / GST_MSECOND;
        playing.worstLate = std::max(playing.worstLate, late);
        if (late > period) {
          ++playing.late;
        }
        gst_sample_unref(sample);
      }
      return GST_FLOW_OK;
    };
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &playing, nullptr);
    gst_object_unref(sink);
    GstElement* audioSink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(audioSink, "sync", TRUE, nullptr);
    GstElement* playbin = gst_element_factory_make("playbin", nullptr);
    g_object_set(
        playbin, "uri", std::format("file://{}", path).c_str(), "video-sink", videoSink, "audio-sink", audioSink,
        nullptr
    );
    GstBus* bus = gst_element_get_bus(playbin);
    gst_bus_add_watch(bus, onBus, &playing);
    const auto begin = Clock::now();
    gst_element_set_state(playbin, GST_STATE_PLAYING);
    g_main_loop_run(playing.loop);
    const double took = milliseconds(begin) / 1000;
    gst_element_set_state(playbin, GST_STATE_NULL);
    std::println(
        "{:.1f} s of video played in {:.2f} s: {} of {} frames reached the sink, {} of them more than a frame period "
        "late (worst {:.1f} ms), {} dropped by the sink",
        seconds, took, playing.received, stream->info().frames, playing.late, playing.worstLate, playing.dropped
    );
    gst_object_unref(bus);
    gst_object_unref(playbin);
    g_main_loop_unref(playing.loop);
    return playing.failed ? 1 : 0;
  }
} // namespace bink2bench
