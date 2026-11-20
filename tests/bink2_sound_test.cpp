#include "bink2_sample.h"
#include "media/bink2_stream.h"
#include "media/gstreamer_runtime.h"
#include "media_clips.h"
#include "test_check.h"

#include <chrono>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <print>
#include <vector>

// The sound of a Bink 2 file through the element: gap free, as long as the picture, and the same sound after any seek,
// also to the frames that follow a run without sound and after the track has ended. It needs a real file with sound:
// LUCIOLE_BINK2_AUDIO_SAMPLE names one, and without it the test exits with meson's skip code, 77.
namespace {
  namespace bink = test::bink2;
  using namespace test::media;
  using namespace std::chrono_literals;

  // Sound over its own sink, next to a picture sink that takes everything. Each stream has a queue of its own, as
  // behind any demuxer that pushes every stream from one thread.
  class SoundRig {
  public:
    explicit SoundRig(const fs::path& path) {
      const auto description = std::format(
          "filesrc location=\"{}\" ! luciolebink2demux name=d d.video_0 ! queue ! fakesink sync=false "
          "d.audio_0 ! queue ! appsink name=sound sync=false max-buffers=1 drop=false",
          path.string()
      );
      GError* error = nullptr;
      m_pipeline = gst_parse_launch(description.c_str(), &error);
      TEST_CHECK(m_pipeline != nullptr && error == nullptr);
      m_sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(m_pipeline), "sound"));
    }
    SoundRig(const SoundRig&) = delete;
    SoundRig& operator=(const SoundRig&) = delete;
    ~SoundRig() {
      gst_element_set_state(m_pipeline, GST_STATE_NULL);
      gst_object_unref(m_sink);
      gst_object_unref(m_pipeline);
    }
    bool paused() {
      gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
      const auto result = gst_element_get_state(m_pipeline, nullptr, nullptr, 20 * GST_SECOND);
      if (result != GST_STATE_CHANGE_SUCCESS) {
        GstBus* bus = gst_element_get_bus(m_pipeline);
        if (GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
          GError* error = nullptr;
          gst_message_parse_error(message, &error, nullptr);
          std::println(stderr, "pipeline: {}", error != nullptr ? error->message : "");
          g_clear_error(&error);
          gst_message_unref(message);
        }
        gst_object_unref(bus);
        std::println(stderr, "state change returned {}", static_cast<int>(result));
      }
      return result == GST_STATE_CHANGE_SUCCESS;
    }
    void play() { gst_element_set_state(m_pipeline, GST_STATE_PLAYING); }
    GstSample* pull() { return gst_app_sink_try_pull_sample(m_sink, 120 * GST_SECOND); }
    bool ended() const { return gst_app_sink_is_eos(m_sink) != FALSE; }
    bool seek(GstClockTime time) {
      return gst_element_seek_simple(
                 m_pipeline, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                 static_cast<gint64>(time)
             )
          != FALSE;
    }

  private:
    GstElement* m_pipeline = nullptr;
    GstAppSink* m_sink = nullptr;
  };

  // The 16-bit samples of a buffer, appended at their place: `at` is the index of the first one.
  void place(std::vector<std::int16_t>& samples, std::uint64_t at, GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    TEST_CHECK(gst_buffer_map(buffer, &map, GST_MAP_READ) != FALSE);
    const std::size_t count = map.size / sizeof(std::int16_t);
    if (samples.size() < at + count) {
      samples.resize(at + count);
    }
    std::memcpy(samples.data() + at, map.data, map.size);
    gst_buffer_unmap(buffer, &map);
  }

  // Sound of a file with sound: played through it is gap free, and after a seek it is the same sound at the same
  // time, from the very first buffer, which holds the seek's time.
  void soundFollowsTheSeeks(const fs::path& path) {
    bink::FileReader reader(path);
    auto opened = luciole::media::bink2::Stream::open(reader.reader(), reader.size());
    TEST_CHECK(opened.has_value() && !(*opened)->info().audio.empty());
    const auto track = (*opened)->info().audio[0];
    const auto channels = static_cast<std::uint64_t>(track.channels);
    const std::uint64_t total = (*opened)->soundBefore(0, (*opened)->info().frames);
    SoundRig linear(path);
    TEST_CHECK(linear.paused());
    linear.play();
    std::vector<std::int16_t> all;
    std::uint64_t counted = 0;
    while (GstSample* sample = linear.pull()) {
      GstBuffer* buffer = gst_sample_get_buffer(sample);
      TEST_CHECK(GST_BUFFER_PTS(buffer) == gst_util_uint64_scale(counted, GST_SECOND, track.rate));
      place(all, counted * channels, sample);
      counted += gst_buffer_get_size(buffer) / (2 * channels);
      gst_sample_unref(sample);
    }
    TEST_CHECK(linear.ended());
    std::println("sound played through: {} samples, the index says {}", counted, total);
    TEST_CHECK(counted == total && total > 0);
    std::println("sound: {} samples at {} Hz in {} channels, gap free", total, track.rate, channels);
    // Seeks: into the file, to its very start, to a time after the last sound.
    const double seconds = static_cast<double>(total) / track.rate;
    // And to the first frames with sound that follow a frame without: the block before them is further back.
    std::vector<double> times{0.0, seconds * 0.33, seconds * 0.5, seconds * 0.97};
    for (std::uint64_t frame = 1; frame < (*opened)->info().frames && times.size() < 8; ++frame) {
      const bool silentBefore = (*opened)->soundBefore(0, frame) == (*opened)->soundBefore(0, frame - 1);
      const bool soundHere = (*opened)->soundBefore(0, frame + 1) > (*opened)->soundBefore(0, frame);
      if (silentBefore && soundHere) {
        const auto& v = (*opened)->info();
        times.push_back(static_cast<double>(frame) * v.frameRateDenominator / v.frameRateNumerator);
      }
    }
    TEST_CHECK(times.size() > 4);
    for (const double at : times) {
      const auto asked = static_cast<GstClockTime>(at * GST_SECOND);
      // The picture the seek lands on starts the segment, so the sound is wanted from there.
      const auto& video = (*opened)->info();
      const GstClockTime time = gst_util_uint64_scale(
          gst_util_uint64_scale(
              asked, video.frameRateNumerator, static_cast<std::uint64_t>(video.frameRateDenominator) * GST_SECOND
          ) * video.frameRateDenominator,
          GST_SECOND, video.frameRateNumerator
      );
      SoundRig rig(path);
      TEST_CHECK(rig.paused());
      TEST_CHECK(rig.seek(asked));
      rig.play();
      GstSample* first = rig.pull();
      TEST_CHECK(first != nullptr);
      const GstClockTime pts = GST_BUFFER_PTS(gst_sample_get_buffer(first));
      const GstClockTime length = GST_BUFFER_DURATION(gst_sample_get_buffer(first));
      TEST_CHECK(pts <= time && time < pts + length);
      const std::uint64_t begins = gst_util_uint64_scale(pts, track.rate, GST_SECOND);
      std::vector<std::int16_t> heard;
      place(heard, 0, first);
      TEST_CHECK(std::memcmp(heard.data(), all.data() + begins * channels, heard.size() * sizeof(std::int16_t)) == 0);
      gst_sample_unref(first);
      std::uint64_t next = begins + heard.size() / channels;
      for (int buffers = 0; buffers < 12; ++buffers) {
        GstSample* sample = rig.pull();
        if (sample == nullptr) {
          break;
        }
        TEST_CHECK(
            GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == gst_util_uint64_scale(next, GST_SECOND, track.rate)
        );
        std::vector<std::int16_t> more;
        place(more, 0, sample);
        TEST_CHECK(std::memcmp(more.data(), all.data() + next * channels, more.size() * sizeof(std::int16_t)) == 0);
        next += more.size() / channels;
        gst_sample_unref(sample);
      }
    }
    // A time after the last sound: the sound has ended.
    SoundRig late(path);
    TEST_CHECK(late.paused());
    TEST_CHECK(late.seek(static_cast<GstClockTime>((seconds + 0.5) * GST_SECOND)));
    late.play();
    TEST_CHECK(late.pull() == nullptr && late.ended());
    // The track has ended; a seek back into the sound brings it again, the same as played through.
    TEST_CHECK(late.seek(static_cast<GstClockTime>(2 * GST_SECOND)));
    GstSample* again = late.pull();
    TEST_CHECK(again != nullptr);
    {
      const auto begins = gst_util_uint64_scale(GST_BUFFER_PTS(gst_sample_get_buffer(again)), track.rate, GST_SECOND);
      std::vector<std::int16_t> heard;
      place(heard, 0, again);
      TEST_CHECK(std::memcmp(heard.data(), all.data() + begins * channels, heard.size() * sizeof(std::int16_t)) == 0);
    }
    gst_sample_unref(again);
    std::println("sound after seeks equals the sound played through");
  }
} // namespace

int main() {
  Scratch scratch;
  TEST_CHECK(luciole::media::startProblem().empty());
  const auto sample = bink::sampleFrom(bink::kAudioSampleVariable);
  soundFollowsTheSeeks(bink::needSample(sample, bink::kAudioSampleVariable));
  return 0;
}
