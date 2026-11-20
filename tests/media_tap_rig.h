#pragma once

#include "media/media_player.h"
#include "media/pipeline_thread.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

// What the audio tap tests share: the tap's rate and window length, a WAV writer and a player with its tap on.
namespace test::media {
  inline constexpr int kTapRate = 22050;
  inline constexpr int kTapLength = 2048;

  // A 16-bit PCM WAV of `seconds` at `rate`, each channel's value at a time from `sample`, in -1 to 1.
  inline void writeWav(
      const fs::path& path, int rate, int channels, double seconds, const std::function<double(int, double)>& sample
  ) {
    const auto frames = static_cast<std::uint32_t>(seconds * rate);
    const auto dataBytes = frames * static_cast<std::uint32_t>(channels) * 2;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + dataBytes);
    const auto put = [&bytes](std::uint32_t value, int size) {
      for (int i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    };
    const auto tag = [&bytes](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
    const auto channelCount = static_cast<std::uint32_t>(channels);
    tag("RIFF");
    put(36 + dataBytes, 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    put(1, 2); // PCM
    put(channelCount, 2);
    put(static_cast<std::uint32_t>(rate), 4);
    put(static_cast<std::uint32_t>(rate) * channelCount * 2, 4);
    put(channelCount * 2, 2);
    put(16, 2);
    tag("data");
    put(dataBytes, 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
      for (int channel = 0; channel < channels; ++channel) {
        const double value = std::clamp(sample(channel, static_cast<double>(i) / rate), -1.0, 1.0);
        put(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32767))), 2);
      }
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::ssize(bytes));
  }

  struct Tapped {
    luciole::MediaPlayer player;
    std::vector<float> samples = std::vector<float>(kTapLength);
    luciole::AudioWindow window;

    explicit Tapped(luciole::AudioOutput output = luciole::AudioOutput::Silent) : player(output) {
      TEST_CHECK(player.startAudioTap(kTapRate, kTapLength));
    }
    luciole::AudioWindow copy() {
      window = player.copyAudioWindow(samples);
      return window;
    }
    std::span<const float> valid() const { return std::span(samples).first(static_cast<std::size_t>(window.count)); }
    // Waits for a full window that moved since the last copy.
    bool full(Pump& pump) {
      return pump.until([this] { return copy().fresh && window.count == kTapLength; });
    }
  };
  inline bool reached(Pump& pump, const luciole::MediaPlayer& player, luciole::MediaState state) {
    return pump.until([&] { return player.state() == state || player.state() == luciole::MediaState::Error; })
        && player.state() == state;
  }

  // Waits until every job already posted to the pipeline thread (such as a prior player's teardown) finishes.
  inline void drainPipelineThread(Pump& pump) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    luciole::media::runOnPipelineThread([done] { done->store(true); });
    TEST_CHECK(pump.until([&done] { return done->load(); }));
  }

  // What reached the stand-in for the sound device, and the buffers that did not start where the one before ended.
  struct Heard {
    std::mutex lock;
    std::vector<std::uint8_t> bytes;
    int gaps = 0;
    GstClockTime next = GST_CLOCK_TIME_NONE;
  };
  inline Heard& heard() {
    static Heard instance;
    return instance;
  }

  inline GstPadProbeReturn record(GstPad* /*pad*/, GstPadProbeInfo* info, gpointer /*data*/) {
    auto& out = heard();
    const std::scoped_lock lock(out.lock);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
      out.next = GST_CLOCK_TIME_NONE; // a new segment or a flush
      return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(out.next) && (pts > out.next + GST_MSECOND || pts + GST_MSECOND < out.next)) {
      ++out.gaps;
    }
    out.next = pts + GST_BUFFER_DURATION(buffer);
    GstMapInfo map;
    TEST_CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ) == TRUE);
    out.bytes.insert(out.bytes.end(), map.data, map.data + map.size);
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
  }

  // A sound output that takes the sound well before it is heard, as a device's ring buffer does, through a queue of
  // 300 ms; it records what it plays and makes no sound.
  inline GType lateSinkType() {
    static const GType type = [] {
      const auto classInit = [](gpointer klass, gpointer /*data*/) {
        gst_element_class_set_static_metadata(
            GST_ELEMENT_CLASS(klass), "Late sink", "Sink/Audio", "Takes sound early and plays nothing", "tests"
        );
      };
      const auto instanceInit = [](GTypeInstance* instance, gpointer /*klass*/) {
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* sink = gst_element_factory_make("appsink", nullptr);
        g_object_set(queue, "max-size-time", 300 * GST_MSECOND, "max-size-buffers", 0U, "max-size-bytes", 0U, nullptr);
        g_object_set(
            sink, "sync", TRUE, "max-buffers", 1U, "drop", TRUE, "wait-on-eos", FALSE, "enable-last-sample", FALSE,
            nullptr
        );
        gst_bin_add_many(GST_BIN(instance), queue, sink, nullptr);
        TEST_CHECK(gst_element_link(queue, sink) == TRUE);
        GstPad* entry = gst_element_get_static_pad(queue, "sink");
        TEST_CHECK(gst_element_add_pad(GST_ELEMENT(instance), gst_ghost_pad_new("sink", entry)) == TRUE);
        gst_object_unref(entry);
        GstPad* played = gst_element_get_static_pad(sink, "sink");
        const auto watched = static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH
        );
        (void)gst_pad_add_probe(played, watched, record, nullptr, nullptr);
        gst_object_unref(played);
      };
      return g_type_register_static_simple(
          GST_TYPE_BIN, "LucioleLateAudioSink", sizeof(GstBinClass), classInit, sizeof(GstBin), instanceInit,
          GTypeFlags{}
      );
    }();
    return type;
  }

  // Makes the system's sound output the late sink for the tests that follow, and checks that it is what gets made.
  inline void useLateSoundOutput() {
    TEST_CHECK(gst_element_register(nullptr, "autoaudiosink", GST_RANK_NONE, lateSinkType()) == TRUE);
    GstElement* sink = gst_element_factory_make("autoaudiosink", nullptr);
    TEST_CHECK(sink != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(sink, lateSinkType()));
    if (sink != nullptr) {
      gst_object_unref(gst_object_ref_sink(sink));
    }
  }
} // namespace test::media
