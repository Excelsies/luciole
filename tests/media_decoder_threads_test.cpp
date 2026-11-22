#include "media/decoder_threads.h"
#include "media/gstreamer_runtime.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>

// A software video decoder gets the threads its stream needs and no more: each extra frame thread holds a decoded
// frame and a codec context, which at 4K is tens of megabytes.
namespace {
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::media::decoderThreads;
  using namespace test::media;

  constexpr int kCores = 24;

  void countsThreadsFromThePixelRate() {
    TEST_CHECK(decoderThreads(160, 120, 10, 1, kCores) == 1);
    TEST_CHECK(decoderThreads(1920, 1080, 30, 1, kCores) == 2);
    TEST_CHECK(decoderThreads(1920, 1080, 60, 1, kCores) == 3);
    TEST_CHECK(decoderThreads(3840, 2160, 30, 1, kCores) == 5);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, kCores) == 9);
    // 29.97 and 59.94 frames a second are the same cost as 30 and 60.
    TEST_CHECK(decoderThreads(3840, 2160, 30000, 1001, kCores) == 5);
  }

  // A stream deeper than 8 bits per sample is counted at one and a half times its pixel rate.
  void countsDeeperStreamsAtOneAndAHalfTimes() {
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, kCores, 10) == 13);
    TEST_CHECK(decoderThreads(3840, 2160, 30, 1, kCores, 10) == 7);
    TEST_CHECK(decoderThreads(3840, 2160, 30, 1, kCores, 8) == 5);
    TEST_CHECK(decoderThreads(3840, 2160, 30, 1, kCores, 0) == 5);
    TEST_CHECK(decoderThreads(1280, 720, 30, 1, kCores, 12) == 1);
  }

  void boundsThreadsByTheCoresAndTheDecoderLimit() {
    TEST_CHECK(decoderThreads(7680, 4320, 60, 1, kCores) == 16);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, 4) == 4);
    TEST_CHECK(decoderThreads(3840, 2160, 60, 1, 1) == 1);
  }

  // A stream that states a size and a rate beyond what an integer or a double of pixels holds.
  void survivesCraftedValues() {
    constexpr int kMost = std::numeric_limits<int>::max();
    TEST_CHECK(decoderThreads(kMost, kMost, kMost, 1, kCores) == 16);
    TEST_CHECK(decoderThreads(kMost, kMost, kMost, 1, 4) == 4);
    TEST_CHECK(decoderThreads(kMost, kMost, 1, kMost, kCores) >= 1);
    TEST_CHECK(decoderThreads(kMost, 1, kMost, 1, kCores) == 16);
  }

  void leavesUnknownStreamsToTheDecoder() {
    TEST_CHECK(decoderThreads(0, 0, 30, 1, kCores) == 0);
    // No count of cores: the decoder keeps its own choice.
    TEST_CHECK(decoderThreads(1920, 1080, 30, 1, 0) == 0);
    // No frame rate: counted as 60.
    TEST_CHECK(decoderThreads(1920, 1080, 0, 0, kCores) == 3);
  }

  struct Decoder {
    std::string element;
    int threads = -1; // what the element's thread property holds, -1 when it has none
  };

  // The software video decoder a playbin builds for the clip, and the value of its thread property once the caps
  // reached it.
  std::optional<Decoder> decoderOf(const fs::path& clip) {
    GstElement* playbin = gst_element_factory_make("playbin", nullptr);
    GstElement* video = gst_element_factory_make("fakesink", nullptr);
    GstElement* audio = gst_element_factory_make("fakesink", nullptr);
    TEST_CHECK(playbin != nullptr && video != nullptr && audio != nullptr);
    gst_object_ref_sink(playbin);
    gchar* converted = g_filename_to_uri(clip.c_str(), nullptr, nullptr);
    const std::string uri = converted;
    g_free(converted);
    g_object_set(playbin, "uri", uri.c_str(), "flags", 0x1U, "video-sink", video, "audio-sink", audio, nullptr);
    luciole::media::refuseNetwork(playbin);
    (void)gst_element_set_state(playbin, GST_STATE_PAUSED);
    std::optional<Decoder> found;
    if (gst_element_get_state(playbin, nullptr, nullptr, 30 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS) {
      GstIterator* items = gst_bin_iterate_recurse(GST_BIN(playbin));
      GValue value = G_VALUE_INIT;
      // A playbin holds a few dozen elements.
      for (int step = 0; step < 4096 && gst_iterator_next(items, &value) == GST_ITERATOR_OK; ++step) {
        auto* element = GST_ELEMENT(g_value_get_object(&value));
        GstElementFactory* factory = gst_element_get_factory(element);
        const char* klass =
            factory != nullptr ? gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS) : "";
        if (std::string_view(klass).contains("Decoder") && std::string_view(klass).contains("Video")) {
          found = Decoder{.element = GST_OBJECT_NAME(factory)};
          for (const char* name : {"max-threads", "n-threads", "threads"}) {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), name) != nullptr) {
              gint threads = 0;
              g_object_get(element, name, &threads, nullptr);
              found->threads = threads;
              break;
            }
          }
        }
        g_value_reset(&value);
      }
      g_value_unset(&value);
      gst_iterator_free(items);
    }
    (void)gst_element_set_state(playbin, GST_STATE_NULL);
    gst_object_unref(playbin);
    return found;
  }

  bool writeH264(const fs::path& path, int width, int height, int fps, int frames) {
    return writeLaunched(
        std::format(
            "videotestsrc num-buffers={} pattern=ball ! video/x-raw,format=I420,width={},height={},framerate={}/1 ! "
            "x264enc speed-preset=ultrafast key-int-max={} ! h264parse ! mp4mux ! filesink location=\"{}\"",
            frames, width, height, fps, fps, path.string()
        )
    );
  }

  // A 10-bit HEVC clip made with ffmpeg when it is there: its decoder gets the count of one and a half times the rate.
  void givesADeeperStreamMoreThreads(Scratch& scratch) {
    const auto clip = scratch.path("deep.mp4");
    const auto make = std::format(
        "ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=s=3840x2160:r=30:d=1 -c:v libx265 -preset "
        "ultrafast -x265-params log-level=error -pix_fmt yuv420p10le -an \"{}\"",
        clip.string()
    );
    if (std::system(make.c_str()) != 0 || !fs::exists(clip)) {
      std::println("a 10-bit stream is not checked: ffmpeg made no file");
      return;
    }
    const auto decoder = decoderOf(clip);
    TEST_CHECK(decoder.has_value());
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    std::println("10-bit 4K30: {} has {} threads", decoder->element, decoder->threads);
    TEST_CHECK(decoder->threads == decoderThreads(3840, 2160, 30, 1, cores, 10));
  }

  // The decoder gets the count the pure function gives for the stream, read from the element itself.
  void givesEachDecoderTheThreadsItsStreamNeeds(Scratch& scratch) {
    struct Case {
      int width;
      int height;
      int fps;
    };
    for (const Case& item : {Case{1920, 1080, 30}, Case{3840, 2160, 30}, Case{1280, 720, 10}, Case{1920, 1080, 60}}) {
      const auto clip = scratch.path(std::format("clip-{}-{}.mp4", item.width, item.fps).c_str());
      TEST_CHECK(writeH264(clip, item.width, item.height, item.fps, item.fps));
      const auto decoder = decoderOf(clip);
      TEST_CHECK(decoder.has_value());
      const int expected =
          decoderThreads(item.width, item.height, item.fps, 1, static_cast<int>(std::thread::hardware_concurrency()));
      std::println(
          "{}x{} at {}: {} has {} threads", item.width, item.height, item.fps, decoder->element, decoder->threads
      );
      TEST_CHECK(decoder->threads == expected);
    }
  }
} // namespace

int main() {
  countsThreadsFromThePixelRate();
  countsDeeperStreamsAtOneAndAHalfTimes();
  boundsThreadsByTheCoresAndTheDecoderLimit();
  survivesCraftedValues();
  leavesUnknownStreamsToTheDecoder();
  if (!MediaPlayer::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  // Starts GStreamer as the player does, which also puts decoders on a graphics device behind the software ones.
  TEST_CHECK(luciole::media::startProblem().empty());
  if (const auto missing = missingElements(
          {"videotestsrc", "x264enc", "h264parse", "mp4mux", "filesink", "avdec_h264", "playbin", "appsink",
           "videoconvertscale", "fakesink"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  givesEachDecoderTheThreadsItsStreamNeeds(scratch);
  givesADeeperStreamMoreThreads(scratch);
  return 0;
}
