#pragma once

#include "media/media_player.h"
#include "media_clips.h"
#include "media_tap_rig.h"
#include "test_check.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <gst/base/gstbasetransform.h>
#include <mutex>
#include <thread>
#include <vector>

// What the loop and seek tests share: the clips they play, a watcher that counts how often the file came round, and
// a sound output that plays on the clock and makes no sound, records when each buffer was due, counts the seeks that
// reach it and can refuse them, all or the next few, or take its time over them.
namespace test::media {
  using Clock = std::chrono::steady_clock;

  inline constexpr double kTone = 0.8; // seconds in the clips most tests loop
  inline constexpr double kNearStart = 0.35;

  struct Clips {
    fs::path tone; // 0.8 s of sine
    fs::path tiny; // 0.02 s
    fs::path empty;
    fs::path sound;   // 3 s of sine
    fs::path video;   // 3 s of picture with sound
    fs::path picture; // 3 s of picture alone
  };

  // Watches a player: every state it reported and how many times the file came round again.
  struct Watched {
    luciole::MediaPlayer player;
    bool ended = false;
    bool failed = false;
    int passes = 0;
    int endedNotices = 0; // changed callbacks that found the state Ended
    bool late = false;    // the position passed the middle since the last wrap

    explicit Watched(luciole::AudioOutput output = luciole::AudioOutput::Silent) : player(output) {
      player.setChangedCallback([this] {
        ended = ended || player.state() == luciole::MediaState::Ended;
        endedNotices += player.state() == luciole::MediaState::Ended ? 1 : 0;
        failed = failed || player.state() == luciole::MediaState::Error;
      });
    }
    // Counts a pass each time the position returns near zero after passing the middle.
    void sample() {
      const double at = player.position();
      if (at > kTone * 0.5) {
        late = true;
      } else if (late && at < kNearStart) {
        late = false;
        ++passes;
      }
    }
    bool passed(Pump& pump, int count) {
      return pump.until([&] {
        sample();
        return passes >= count || ended || failed;
      }) && passes >= count;
    }
  };

  // Opens the tone with loop as given and plays it from the start.
  inline void playTone(Pump& pump, Watched& watched, const Clips& clips, bool loop) {
    watched.player.setLoop(loop);
    TEST_CHECK(watched.player.open(clips.tone.string()));
    TEST_CHECK(reached(pump, watched.player, luciole::MediaState::Paused));
    watched.player.play();
  }

  // What reached the stand-in output, with the time each buffer was due.
  struct Buffer {
    Clock::time_point at;
    GstClockTime pts;
    GstClockTime duration;
  };
  struct Output {
    std::mutex lock;
    std::vector<Buffer> buffers;
    std::atomic<bool> refuseSeeks{false};
    std::atomic<int> refuseNext{0};      // the next this many seek events are refused
    std::atomic<int> refuseVideoNext{0}; // the next this many seeks reaching a video stream's filter are refused
    std::atomic<int> seekDelayMs{0};     // each seek event waits this long before the output takes or refuses it
    std::atomic<int> seeks{0};           // seek events that reached the output, taken or refused
  };
  inline Output& output() {
    static Output instance;
    return instance;
  }

  inline GstFlowReturn onSample(GstElement* sink, gpointer /*data*/) {
    GstSample* sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (sample != nullptr) {
      GstBuffer* buffer = gst_sample_get_buffer(sample);
      auto& out = output();
      const std::scoped_lock lock(out.lock);
      out.buffers.push_back({Clock::now(), GST_BUFFER_PTS(buffer), GST_BUFFER_DURATION(buffer)});
      gst_sample_unref(sample);
    }
    return GST_FLOW_OK;
  }

  inline GstElementClass* recordingParentClass = nullptr;

  inline gboolean sendEvent(GstElement* element, GstEvent* event) {
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEEK) {
      ++output().seeks;
      if (const int delay = output().seekDelayMs.load(); delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }
      if (output().refuseSeeks.load() || output().refuseNext.fetch_sub(1) > 0) {
        gst_event_unref(event);
        return FALSE;
      }
    }
    return recordingParentClass->send_event(element, event);
  }

  // A sound output that plays on the clock and makes no sound; it records its buffers and can refuse seeks.
  inline GType recordingSinkType() {
    static const GType type = [] {
      const auto classInit = [](gpointer klass, gpointer /*data*/) {
        gst_element_class_set_static_metadata(
            GST_ELEMENT_CLASS(klass), "Recording sink", "Sink/Audio", "Records what it plays and plays nothing", "tests"
        );
        recordingParentClass = GST_ELEMENT_CLASS(g_type_class_peek_parent(klass));
        GST_ELEMENT_CLASS(klass)->send_event = sendEvent;
      };
      const auto instanceInit = [](GTypeInstance* instance, gpointer /*klass*/) {
        GstElement* sink = gst_element_factory_make("appsink", nullptr);
        g_object_set(sink, "sync", TRUE, "emit-signals", TRUE, "wait-on-eos", FALSE, nullptr);
        g_signal_connect(sink, "new-sample", G_CALLBACK(onSample), nullptr);
        gst_bin_add(GST_BIN(instance), sink);
        GstPad* entry = gst_element_get_static_pad(sink, "sink");
        TEST_CHECK(gst_element_add_pad(GST_ELEMENT(instance), gst_ghost_pad_new("sink", entry)) == TRUE);
        gst_object_unref(entry);
      };
      return g_type_register_static_simple(
          GST_TYPE_BIN, "LucioleRecordingAudioSink", sizeof(GstBinClass), classInit, sizeof(GstBin), instanceInit,
          GTypeFlags{}
      );
    }();
    return type;
  }

  inline void useRecordingOutput() {
    TEST_CHECK(gst_element_register(nullptr, "autoaudiosink", GST_RANK_NONE, recordingSinkType()) == TRUE);
    GstElement* sink = gst_element_factory_make("autoaudiosink", nullptr);
    TEST_CHECK(sink != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(sink, recordingSinkType()));
    if (sink != nullptr) {
      gst_object_unref(gst_object_ref_sink(sink));
    }
  }

  // The seek events a video stream gets are counted where they reach the frame sink's caps filter, since the sound
  // output sees a seek only when it is the first sink asked. The filter keeps its work, counts upstream seeks and can
  // refuse them.
  inline GstBaseTransformClass* countingParentClass = nullptr;

  inline gboolean onFrameSeek(GstBaseTransform* element, GstEvent* event) {
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEEK) {
      ++output().seeks;
      if (const int delay = output().seekDelayMs.load(); delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }
      if (output().refuseVideoNext.fetch_sub(1) > 0) {
        gst_event_unref(event);
        return FALSE;
      }
    }
    return countingParentClass->src_event(element, event);
  }

  inline GType countingFilterType() {
    static const GType type = [] {
      GstElementFactory* factory = gst_element_factory_find("capsfilter");
      TEST_CHECK(factory != nullptr);
      const GType parent = gst_element_factory_get_element_type(factory);
      gst_object_unref(factory);
      GTypeQuery size{};
      g_type_query(parent, &size);
      const auto classInit = [](gpointer klass, gpointer /*data*/) {
        countingParentClass = GST_BASE_TRANSFORM_CLASS(g_type_class_peek_parent(klass));
        GST_BASE_TRANSFORM_CLASS(klass)->src_event = onFrameSeek;
      };
      return g_type_register_static_simple(
          parent, "LucioleCountingCapsFilter", size.class_size, classInit, size.instance_size, nullptr, GTypeFlags{}
      );
    }();
    return type;
  }

  // From now on every video stream counts its seeks in output().seeks.
  inline void countVideoSeeks() {
    TEST_CHECK(gst_element_register(nullptr, "capsfilter", GST_RANK_NONE, countingFilterType()) == TRUE);
    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    TEST_CHECK(filter != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(filter, countingFilterType()));
    if (filter != nullptr) {
      gst_object_unref(gst_object_ref_sink(filter));
    }
  }

  inline void forgetBuffers() {
    const std::scoped_lock lock(output().lock);
    output().buffers.clear();
  }
} // namespace test::media
