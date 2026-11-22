#include "bink2_sample.h"
#include "media/bink2_demux.h"
#include "media/bink2_stream.h"
#include "media/gstreamer_runtime.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/video/video.h>
#include <print>
#include <thread>
#include <vector>

extern "C" {
#include <bink2.h>
}

// The Bink 2 element on its own, between a file source and an application sink: what it delivers, how it answers
// seeks and a flush, and the end. It needs a real Bink 2 file: LUCIOLE_BINK2_SAMPLE names one, and without it the
// test exits with meson's skip code, 77. Sound is in bink2_sound_test. What needs no file is in bink2_refusal_test.
namespace {
  namespace bink = test::bink2;
  using namespace test::media;
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;

  // A pipeline from the file through the element to the picture pad's application sink.
  class Rig {
  public:
    explicit Rig(const fs::path& path) {
      m_pipeline = gst_pipeline_new(nullptr);
      GstElement* source = gst_element_factory_make("filesrc", nullptr);
      GstElement* demux = gst_element_factory_make("luciolebink2demux", nullptr);
      GstElement* sink = gst_element_factory_make("appsink", "sink");
      TEST_CHECK(m_pipeline != nullptr && source != nullptr && demux != nullptr && sink != nullptr);
      g_object_set(source, "location", path.c_str(), nullptr);
      g_object_set(sink, "sync", FALSE, "max-buffers", 1U, "drop", FALSE, nullptr);
      gst_bin_add_many(GST_BIN(m_pipeline), source, demux, sink, nullptr);
      TEST_CHECK(gst_element_link(source, demux) != FALSE);
      // Each start of the element makes its picture pad again, so each is linked as it appears.
      g_signal_connect(
          demux, "pad-added", G_CALLBACK(+[](GstElement* /*element*/, GstPad* pad, gpointer data) {
            if (std::string_view(GST_PAD_NAME(pad)) == "video_0") {
              GstPad* entry = gst_element_get_static_pad(GST_ELEMENT(data), "sink");
              TEST_CHECK(gst_pad_link(pad, entry) == GST_PAD_LINK_OK);
              gst_object_unref(entry);
            }
          }),
          sink
      );
      m_sink = GST_APP_SINK(gst_object_ref(sink));
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    ~Rig() {
      gst_element_set_state(m_pipeline, GST_STATE_NULL);
      gst_object_unref(m_sink);
      gst_object_unref(m_pipeline);
    }

    GstElement* pipeline() const { return m_pipeline; }
    bool paused() {
      gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
      return gst_element_get_state(m_pipeline, nullptr, nullptr, 60 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS;
    }
    void play() { gst_element_set_state(m_pipeline, GST_STATE_PLAYING); }
    bool ended() const { return gst_app_sink_is_eos(m_sink) != FALSE; }
    // The next picture, or null at the end or when nothing comes within `wait`.
    GstSample* pull(std::chrono::milliseconds wait = 120s) {
      return gst_app_sink_try_pull_sample(m_sink, static_cast<GstClockTime>(wait.count()) * GST_MSECOND);
    }
    // The picture the paused pipeline settled on, or null.
    GstSample* preroll(std::chrono::milliseconds wait = 120s) {
      return gst_app_sink_try_pull_preroll(m_sink, static_cast<GstClockTime>(wait.count()) * GST_MSECOND);
    }
    bool seek(double seconds, GstSeekFlags precision) {
      return gst_element_seek_simple(
                 m_pipeline, GST_FORMAT_TIME, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | precision),
                 static_cast<gint64>(seconds * GST_SECOND)
             )
          != FALSE;
    }
    // The first error on the bus within `wait`, or empty.
    std::string error(std::chrono::milliseconds wait) {
      GstBus* bus = gst_element_get_bus(m_pipeline);
      GstMessage* message =
          gst_bus_timed_pop_filtered(bus, static_cast<GstClockTime>(wait.count()) * GST_MSECOND, GST_MESSAGE_ERROR);
      gst_object_unref(bus);
      if (message == nullptr) {
        return {};
      }
      GError* problem = nullptr;
      gst_message_parse_error(message, &problem, nullptr);
      std::string text = problem != nullptr ? problem->message : "";
      g_clear_error(&problem);
      gst_message_unref(message);
      return text;
    }

  private:
    GstElement* m_pipeline = nullptr;
    GstAppSink* m_sink = nullptr;
  };

  GstClockTime ptsOf(GstSample* sample) { return GST_BUFFER_PTS(gst_sample_get_buffer(sample)); }

  // The element stopped and started again plays the file from its start, as playbin does when a file is opened
  // again.
  void startsAgainAfterStopping(const fs::path& path) {
    Rig rig(path);
    TEST_CHECK(rig.paused());
    GstSample* first = rig.preroll(120s);
    TEST_CHECK(first != nullptr && ptsOf(first) == 0);
    gst_sample_unref(first);
    TEST_CHECK(gst_element_set_state(rig.pipeline(), GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    TEST_CHECK(rig.paused());
    GstSample* again = rig.preroll(120s);
    TEST_CHECK(again != nullptr && ptsOf(again) == 0);
    gst_sample_unref(again);
  }

  // A reader that keeps every picture it is given gets only a few before the element waits for one back: the pictures
  // are 12 MB each at 4K.
  void handsOutOnlyAFewPictures(const fs::path& path) {
    Rig rig(path);
    TEST_CHECK(rig.paused());
    rig.play();
    std::vector<GstSample*> held;
    // Every picture of the pool goes to this reader, which keeps it, however long the decoder takes for each.
    for (int i = 0; i < luciole::media::bink2::kPoolMaximum; ++i) {
      GstSample* sample = rig.pull(60s);
      TEST_CHECK(sample != nullptr);
      held.push_back(sample);
    }
    // The pool is empty: no picture comes while all are held.
    TEST_CHECK(rig.pull(2s) == nullptr);
    std::println("the element handed out {} pictures to a reader that kept them", held.size());
    for (GstSample* sample : held) {
      gst_sample_unref(sample);
    }
  }

  // The pictures in order, timed by the frame rate and laid out as the element says, and the end after the last: the
  // start of the file, then, past a seek, its last frames.
  void deliversEveryPictureInOrder(const fs::path& path) {
    bink::FileReader reader(path);
    auto opened = luciole::media::bink2::Stream::open(reader.reader(), reader.size());
    TEST_CHECK(opened.has_value());
    const auto& info = (*opened)->info();
    constexpr std::uint64_t kStart = 120;
    constexpr std::uint64_t kTail = 40;
    Rig rig(path);
    TEST_CHECK(rig.paused());
    rig.play();
    std::uint64_t frame = 0;
    std::uint64_t count = 0;
    GstClockTime last = 0;
    const auto next = [&] {
      GstSample* sample = rig.pull();
      TEST_CHECK(sample != nullptr);
      const GstClockTime pts = ptsOf(sample);
      TEST_CHECK(pts == gst_util_uint64_scale(frame * info.frameRateDenominator, GST_SECOND, info.frameRateNumerator));
      TEST_CHECK(GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)) == frame);
      TEST_CHECK(count == 0 || pts > last);
      last = pts;
      ++count;
      if (frame == 0) {
        GstVideoInfo video;
        TEST_CHECK(gst_video_info_from_caps(&video, gst_sample_get_caps(sample)) != FALSE);
        TEST_CHECK(GST_VIDEO_INFO_FORMAT(&video) == GST_VIDEO_FORMAT_I420);
        TEST_CHECK(GST_VIDEO_INFO_WIDTH(&video) == info.width && GST_VIDEO_INFO_HEIGHT(&video) == info.height);
        TEST_CHECK(video.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255);
        GstVideoMeta* meta = gst_buffer_get_video_meta(gst_sample_get_buffer(sample));
        TEST_CHECK(meta != nullptr && meta->stride[0] == (*opened)->layout().stride[0]);
      }
      gst_sample_unref(sample);
    };
    for (; frame < std::min(info.frames, kStart); ++frame) {
      next();
    }
    if (info.frames > kStart + kTail) {
      frame = info.frames - kTail;
      TEST_CHECK(rig.seek(
          static_cast<double>(frame) * info.frameRateDenominator / info.frameRateNumerator, GST_SEEK_FLAG_ACCURATE
      ));
      // A picture that was already waiting in the sink when the seek went out may still come first.
      for (int stale = 0; stale < 8; ++stale) {
        GstSample* sample = rig.pull();
        TEST_CHECK(sample != nullptr);
        const bool landed = GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)) == frame;
        gst_sample_unref(sample);
        if (landed) {
          break;
        }
      }
      ++frame;
      ++count;
    }
    for (; frame < info.frames; ++frame) {
      next();
    }
    // The end: nothing more, and the sink knows it.
    TEST_CHECK(rig.pull(5s) == nullptr);
    TEST_CHECK(rig.ended());
    std::println("{} pictures in order, then the end", count);
  }

  // The bytes of a picture against the library's own decode of the same frame.
  void
  picturesAreTheLibrarysPictures(const fs::path& path, const std::vector<std::uint32_t>& keys, std::uint32_t frames) {
    bink::FileReader reader(path);
    auto readFile = reader.reader();
    const auto read = [](void* user, unsigned long offset, void* destination, unsigned long bytes) {
      return (*static_cast<luciole::media::bink2::Stream::Read*>(user))(offset, destination, bytes) ? 0 : -1;
    };
    bink2 library = bink2_open_callbacks(read, &readFile, static_cast<unsigned long>(reader.size()));
    TEST_CHECK(library != nullptr);
    TEST_CHECK(bink2_enable_video(library, 1) == 0);
    Rig rig(path);
    TEST_CHECK(rig.paused());
    // An exact seek into a run after a key frame: the element decodes forward from the key frame to the frame.
    const std::uint32_t key = keys[keys.size() / 2];
    const std::uint32_t target = std::min(key + 7, frames - 1);
    GstSample* first = rig.preroll(120s);
    TEST_CHECK(first != nullptr);
    gst_sample_unref(first);
    const auto fps = static_cast<double>(bink2_info_all(library, nullptr, nullptr, nullptr) == 0 ? 1 : 0);
    (void)fps;
    unsigned long frameCount = 0;
    double microseconds = 0;
    TEST_CHECK(bink2_info_all(library, nullptr, &frameCount, &microseconds) == 0);
    const double seconds = target * microseconds / 1e6;
    TEST_CHECK(rig.seek(seconds + microseconds / 4e6, GST_SEEK_FLAG_ACCURATE));
    GstSample* sample = rig.preroll(60s);
    TEST_CHECK(sample != nullptr);
    TEST_CHECK(GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)) == target);
    TEST_CHECK(bink2_seek_keyframe(library, key) >= 0);
    for (std::uint32_t frame = key; frame < target; ++frame) {
      TEST_CHECK(bink2_next(library) >= 0);
    }
    GstVideoInfo video;
    TEST_CHECK(gst_video_info_from_caps(&video, gst_sample_get_caps(sample)) != FALSE);
    GstVideoFrame mapped;
    TEST_CHECK(gst_video_frame_map(&mapped, &video, gst_sample_get_buffer(sample), GST_MAP_READ) != FALSE);
    for (int plane = 0; plane < 3; ++plane) {
      const auto* wanted = bink2_get_plane(library, plane);
      const int rows = GST_VIDEO_FRAME_COMP_HEIGHT(&mapped, plane);
      const int width = GST_VIDEO_FRAME_COMP_WIDTH(&mapped, plane);
      const int stride = bink2_get_stride(library, plane);
      const auto* got = static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, plane));
      const int gotStride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, plane);
      for (int row = 0; row < rows; ++row) {
        TEST_CHECK(
            std::memcmp(
                got + static_cast<std::ptrdiff_t>(row) * gotStride, wanted + static_cast<std::ptrdiff_t>(row) * stride,
                static_cast<std::size_t>(width)
            )
            == 0
        );
      }
    }
    gst_video_frame_unmap(&mapped);
    gst_sample_unref(sample);
    bink2_close(library);
    std::println("frame {} after a seek equals the library's decode", target);
  }

  void keyFrameSeeksLandOnKeyFrames(
      const fs::path& path, const std::vector<std::uint32_t>& keys, std::uint32_t frames, double fps
  ) {
    Rig rig(path);
    TEST_CHECK(rig.paused());
    const auto frameOf = [](GstSample* sample) { return GST_BUFFER_OFFSET(gst_sample_get_buffer(sample)); };
    const auto nearest = static_cast<GstSeekFlags>(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST);
    const double last = keys.back() / fps;
    for (const double at : {0.0, 0.3, 0.31, 0.55, 0.9}) {
      TEST_CHECK(rig.seek(at * last, nearest));
      GstSample* sample = rig.preroll(120s);
      TEST_CHECK(sample != nullptr);
      const auto frame = frameOf(sample);
      TEST_CHECK(std::ranges::binary_search(keys, static_cast<std::uint32_t>(frame)));
      // The nearest of the key frames: no other is closer to the asked frame.
      const auto asked = static_cast<std::int64_t>(std::floor(at * last * fps));
      for (const auto key : keys) {
        TEST_CHECK(
            std::abs(static_cast<std::int64_t>(key) - asked) >= std::abs(static_cast<std::int64_t>(frame) - asked)
        );
      }
      gst_sample_unref(sample);
    }
    // Before and after the frame just below the last key frame.
    const double below = std::max(0.0, last - 0.5 / fps);
    const auto belowFrame = static_cast<std::uint32_t>(std::floor(below * fps));
    const auto firstKeyAfter = *std::ranges::lower_bound(keys, belowFrame);
    TEST_CHECK(rig.seek(below, static_cast<GstSeekFlags>(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_AFTER)));
    GstSample* after = rig.preroll(120s);
    TEST_CHECK(after != nullptr && frameOf(after) == firstKeyAfter);
    gst_sample_unref(after);
    if (keys.back() + 2 < frames) {
      TEST_CHECK(
          rig.seek(last + 2.5 / fps, static_cast<GstSeekFlags>(GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_BEFORE))
      );
      GstSample* before = rig.preroll(120s);
      TEST_CHECK(before != nullptr && frameOf(before) == keys.back());
      gst_sample_unref(before);
    }
  }

  // A seek that flushes a long decode forward: the newer seek is answered at once, not after the old one finished.
  void aFlushInterruptsADecodeForward(
      const fs::path& path, const std::vector<std::uint32_t>& keys, std::uint32_t frames, double fps
  ) {
    std::uint32_t from = 0;
    std::uint32_t span = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const std::uint32_t end = i + 1 < keys.size() ? keys[i + 1] : frames;
      if (end - keys[i] > span) {
        span = end - keys[i];
        from = keys[i];
      }
    }
    Rig rig(path);
    TEST_CHECK(rig.paused());
    const auto before = Clock::now();
    TEST_CHECK(rig.seek((from + span - 1 + 0.2) / fps, GST_SEEK_FLAG_ACCURATE));
    GstSample* far = rig.preroll(120s);
    TEST_CHECK(far != nullptr);
    gst_sample_unref(far);
    const auto longTime = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    // Now the same seek, followed at once by one to the start.
    TEST_CHECK(rig.seek((from + span - 1 + 0.2) / fps, GST_SEEK_FLAG_ACCURATE));
    std::this_thread::sleep_for(30ms);
    const auto second = Clock::now();
    TEST_CHECK(rig.seek(0.0, GST_SEEK_FLAG_ACCURATE));
    GstSample* start = rig.preroll(120s);
    TEST_CHECK(start != nullptr);
    // What happened, not how long it took: the newer seek landed on the start, not on the far frame.
    TEST_CHECK(ptsOf(start) == 0 && GST_BUFFER_OFFSET(gst_sample_get_buffer(start)) == 0);
    gst_sample_unref(start);
    const auto shortTime = std::chrono::duration<double, std::milli>(Clock::now() - second).count();
    std::println(
        "exact seek over {} frames: {:.0f} ms; a flush then a seek to the start: {:.0f} ms", span, longTime, shortTime
    );
  }
} // namespace

int main() {
  Scratch scratch;
  TEST_CHECK(luciole::media::startProblem().empty());
  const auto sample = bink::sampleFrom(bink::kSampleVariable);
  const auto& path = bink::needSample(sample, bink::kSampleVariable);
  bink::FileReader reader(path);
  auto opened = luciole::media::bink2::Stream::open(reader.reader(), reader.size());
  TEST_CHECK(opened.has_value());
  const auto& info = (*opened)->info();
  const std::vector<std::uint32_t> keys = (*opened)->keyFrames();
  const double fps = static_cast<double>(info.frameRateNumerator) / info.frameRateDenominator;
  const auto frames = static_cast<std::uint32_t>(info.frames);
  opened->reset();
  startsAgainAfterStopping(path);
  handsOutOnlyAFewPictures(path);
  deliversEveryPictureInOrder(path);
  picturesAreTheLibrarysPictures(path, keys, frames);
  keyFrameSeeksLandOnKeyFrames(path, keys, frames, fps);
  aFlushInterruptsADecodeForward(path, keys, frames, fps);
  std::println("bink2 demux checks passed");
  return 0;
}
