// Times Bink 2 decoding and playback of one file.
//
//   luciole-bink2-bench decode FILE [FRAMES]       milliseconds per frame, decoding in a loop, and the CPU time
//   luciole-bink2-bench adler FILE [FIRST COUNT]   per picture: the Adler-32 of its visible planes, as "crc N 0x..."
//   luciole-bink2-bench sum FILE [FRAMES]          one line per picture: a hash of each plane's visible bytes
//   luciole-bink2-bench scan FILE [FIRST COUNT]    per picture: key frame flag, blockiness, flat share (numbers only)
//   luciole-bink2-bench dump FILE FRAME OUT        the picture FRAME (decoded from its key frame) as raw I420 to OUT
//   luciole-bink2-bench seekto FILE                the time to reach a picture at a quarter, half and three quarters
//   luciole-bink2-bench play FILE [WIDTH HEIGHT]   plays the file once through playbin as the player does
//   luciole-bink2-bench poster FILE [SIDE]         the poster the thumbnail path makes, five times
//   luciole-bink2-bench seek FILE                  the cost of an exact seek over the longest run of frames
//
// Playback counts what the sink received and the frames it dropped because they were late.
#include "media/bink2_poster.h"
#include "media/bink2_stream.h"
#include "media/gstreamer_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <format>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <memory>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
  using Clock = std::chrono::steady_clock;
  using luciole::media::bink2::Planes;
  using luciole::media::bink2::Stream;

  class File {
  public:
    explicit File(const char* path) : m_fd(::open(path, O_RDONLY | O_CLOEXEC)) {
      struct stat status{};
      if (m_fd < 0 || ::fstat(m_fd, &status) != 0) {
        std::println(stderr, "cannot open {}", path);
        std::exit(2);
      }
      m_size = static_cast<std::uint64_t>(status.st_size);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { ::close(m_fd); }
    std::uint64_t size() const { return m_size; }
    Stream::Read reader() const {
      return [this](std::uint64_t offset, void* destination, std::size_t count) {
        auto* out = static_cast<std::uint8_t*>(destination);
        while (count > 0) {
          const auto got = ::pread(m_fd, out, count, static_cast<off_t>(offset));
          if (got <= 0) {
            return false;
          }
          out += got;
          offset += static_cast<std::uint64_t>(got);
          count -= static_cast<std::size_t>(got);
        }
        return true;
      };
    }

  private:
    int m_fd;
    std::uint64_t m_size = 0;
  };

  struct Pictures {
    explicit Pictures(const luciole::media::bink2::Layout& layout) : memory(layout.total) {
      planes.plane = {memory.data(), memory.data() + layout.size[0], memory.data() + layout.size[0] + layout.size[1]};
    }
    std::vector<std::uint8_t> memory;
    Planes planes;
  };

  std::unique_ptr<Stream> open(const File& file) {
    auto stream = Stream::open(file.reader(), file.size());
    if (!stream) {
      std::println(stderr, "{}", stream.error());
      std::exit(2);
    }
    return std::move(*stream);
  }

  // CPU seconds of the whole process, all threads.
  double cpuSeconds() {
    timespec now{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
  }

  double milliseconds(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
  }

  int decode(const char* path, std::uint64_t frames) {
    const File file(path);
    const auto stream = open(file);
    frames = std::min(frames, stream->info().frames);
    Pictures before(stream->layout());
    Pictures now(stream->layout());
    std::vector<double> times;
    const double cpuBegin = cpuSeconds();
    const auto wallBegin = Clock::now();
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      const auto begin = Clock::now();
      const auto done = stream->decode(frame, now.planes, frame == 0 ? nullptr : &before.planes);
      times.push_back(milliseconds(begin));
      if (!done) {
        std::println(stderr, "frame {}: {}", frame, done.error());
        return 1;
      }
      std::swap(before, now);
    }
    const double wall = milliseconds(wallBegin) / 1000;
    const double cpu = cpuSeconds() - cpuBegin;
    std::println(
        "{:.2f} frames per second, {:.2f} s wall, {:.2f} s cpu ({:.2f} cores)", static_cast<double>(frames) / wall,
        wall, cpu, cpu / wall
    );
    std::ranges::sort(times);
    double sum = 0;
    for (const double time : times) {
      sum += time;
    }
    std::println(
        "{} frames: mean {:.2f} ms, median {:.2f}, 99th {:.2f}, slowest {:.2f}", frames,
        sum / static_cast<double>(frames), times[times.size() / 2], times[times.size() * 99 / 100], times.back()
    );
    return 0;
  }

  // FNV-1a over `count` bytes, continuing `hash`.
  std::uint64_t fnv(std::uint64_t hash, const std::uint8_t* bytes, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      hash = (hash ^ bytes[i]) * 1099511628211ULL;
    }
    return hash;
  }

  // The visible bytes of each plane of each picture, hashed: numbers only, for comparing builds.
  int sum(const char* path, std::uint64_t frames) {
    const File file(path);
    const auto stream = open(file);
    frames = std::min(frames, stream->info().frames);
    const auto& info = stream->info();
    const auto& layout = stream->layout();
    Pictures before(layout);
    Pictures now(layout);
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      const auto done = stream->decode(frame, now.planes, frame == 0 ? nullptr : &before.planes);
      if (!done) {
        std::println(stderr, "frame {}: {}", frame, done.error());
        return 1;
      }
      std::string line = std::format("{}", frame);
      for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? info.width : (info.width + 1) / 2;
        const int height = plane == 0 ? info.height : (info.height + 1) / 2;
        std::uint64_t hash = 14695981039346656037ULL;
        for (int row = 0; row < height; ++row) {
          hash =
              fnv(hash,
                  now.planes.plane[static_cast<std::size_t>(plane)]
                      + static_cast<std::size_t>(row) * layout.stride[static_cast<std::size_t>(plane)],
                  static_cast<std::size_t>(width));
        }
        line += std::format(" {:016x}", hash);
      }
      std::println("{}", line);
      std::swap(before, now);
    }
    return 0;
  }

  // Luma measures of a picture, from every fourth row. Blockiness: the mean absolute step across the borders of the
  // 8x8 blocks over the mean step inside them (about 1 in clean pictures). Flat share: the share of 8x8 blocks whose
  // luma spans at most one level.
  struct Look {
    double blockiness = 0;
    double flat = 0;
  };

  Look look(const std::uint8_t* luma, int stride, int width, int height) {
    double across = 0;
    double inside = 0;
    std::uint64_t acrossCount = 0;
    std::uint64_t insideCount = 0;
    std::uint64_t flat = 0;
    std::uint64_t blocks = 0;
    for (int y = 0; y + 8 <= height; y += 4) {
      const std::uint8_t* row = luma + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
      for (int x = 1; x < width; ++x) {
        const int step = std::abs(static_cast<int>(row[x]) - static_cast<int>(row[x - 1]));
        if ((x & 7) == 0) {
          across += step;
          ++acrossCount;
        } else {
          inside += step;
          ++insideCount;
        }
      }
    }
    for (int by = 0; by + 8 <= height; by += 8) {
      for (int bx = 0; bx + 8 <= width; bx += 8) {
        int low = 255;
        int high = 0;
        for (int y = 0; y < 8; ++y) {
          const std::uint8_t* row =
              luma + static_cast<std::size_t>(by + y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(bx);
          for (int x = 0; x < 8; ++x) {
            low = std::min<int>(low, row[x]);
            high = std::max<int>(high, row[x]);
          }
        }
        flat += high - low <= 1 ? 1 : 0;
        ++blocks;
      }
    }
    const double inner = inside / static_cast<double>(std::max<std::uint64_t>(insideCount, 1));
    return {
        .blockiness = (across / static_cast<double>(std::max<std::uint64_t>(acrossCount, 1))) / std::max(inner, 0.01),
        .flat = static_cast<double>(flat) / static_cast<double>(std::max<std::uint64_t>(blocks, 1))
    };
  }

  // Adler-32 starting from zero over the visible bytes of the three planes in turn.
  std::uint32_t
  adler(const Pictures& picture, const luciole::media::bink2::Info& info, const luciole::media::bink2::Layout& layout) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    for (int plane = 0; plane < 3; ++plane) {
      const auto at = static_cast<std::size_t>(plane);
      const int width = plane == 0 ? info.width : (info.width + 1) / 2;
      const int height = plane == 0 ? info.height : (info.height + 1) / 2;
      for (int row = 0; row < height; ++row) {
        const std::uint8_t* bytes =
            picture.planes.plane[at] + static_cast<std::size_t>(row) * static_cast<std::size_t>(layout.stride[at]);
        for (int x = 0; x < width; ++x) {
          low += bytes[x];
          high += low;
          if ((x & 2047) == 2047) {
            low %= 65521;
            high %= 65521;
          }
        }
        low %= 65521;
        high %= 65521;
      }
    }
    return (high << 16) | low;
  }

  // Decodes from the start; hashes the pictures FIRST to FIRST+COUNT-1.
  int adlers(const char* path, std::uint64_t first, std::uint64_t count) {
    const File file(path);
    const auto stream = open(file);
    const auto& layout = stream->layout();
    const std::uint64_t end = std::min(stream->info().frames, first + count);
    Pictures before(layout);
    Pictures now(layout);
    for (std::uint64_t frame = 0; frame < end; ++frame) {
      if (!stream->decode(frame, now.planes, frame == 0 ? nullptr : &before.planes)) {
        std::println("{} error", frame);
        return 1;
      }
      if (frame >= first) {
        std::println("crc {} 0x{:08x}", frame, adler(now, stream->info(), layout));
      }
      std::swap(before, now);
    }
    return 0;
  }

  int scan(const char* path, std::uint64_t first, std::uint64_t count) {
    const File file(path);
    const auto stream = open(file);
    const auto& info = stream->info();
    const auto& layout = stream->layout();
    std::uint64_t from = 0;
    for (const auto key : stream->keyFrames()) {
      if (key <= first) {
        from = key;
      }
    }
    const std::uint64_t end = std::min(info.frames, first + count);
    Pictures before(layout);
    Pictures now(layout);
    std::vector<std::uint32_t> keys(stream->keyFrames().begin(), stream->keyFrames().end());
    for (std::uint64_t frame = from; frame < end; ++frame) {
      if (!stream->decode(frame, now.planes, frame == from ? nullptr : &before.planes)) {
        std::println("{} error", frame);
        return 1;
      }
      if (frame >= first) {
        const auto seen = look(now.planes.plane[0], layout.stride[0], info.width, info.height);
        const bool key = std::ranges::binary_search(keys, static_cast<std::uint32_t>(frame));
        std::println("{} {} {:.3f} {:.4f}", frame, key ? 1 : 0, seen.blockiness, seen.flat);
      }
      std::swap(before, now);
    }
    return 0;
  }

  void writePlanes(
      const Pictures& picture, const luciole::media::bink2::Info& info, const luciole::media::bink2::Layout& layout,
      std::FILE* out
  ) {
    for (int plane = 0; plane < 3; ++plane) {
      const auto at = static_cast<std::size_t>(plane);
      const int width = plane == 0 ? info.width : (info.width + 1) / 2;
      const int height = plane == 0 ? info.height : (info.height + 1) / 2;
      for (int row = 0; row < height; ++row) {
        std::fwrite(
            picture.planes.plane[at] + static_cast<std::size_t>(row) * static_cast<std::size_t>(layout.stride[at]), 1,
            static_cast<std::size_t>(width), out
        );
      }
    }
  }

  // Each picture FIRST to FIRST+COUNT-1, decoded from the key frame before FIRST, as DIR/fNNNNN.yuv.
  int dumpRange(const char* path, std::uint64_t first, std::uint64_t count, const std::string& dir) {
    const File file(path);
    const auto stream = open(file);
    std::uint64_t from = 0;
    for (const auto key : stream->keyFrames()) {
      if (key <= first) {
        from = key;
      }
    }
    Pictures before(stream->layout());
    Pictures now(stream->layout());
    for (std::uint64_t frame = from; frame < first + count; ++frame) {
      if (!stream->decode(frame, now.planes, frame == from ? nullptr : &before.planes)) {
        std::println(stderr, "frame {}: damaged", frame);
        return 1;
      }
      if (frame >= first) {
        std::FILE* out = std::fopen(std::format("{}/f{:05}.yuv", dir, frame).c_str(), "wb");
        if (out == nullptr) {
          return 2;
        }
        writePlanes(now, stream->info(), stream->layout(), out);
        std::fclose(out);
      }
      std::swap(before, now);
    }
    return 0;
  }

  int dump(const char* path, std::uint64_t target, const char* out) {
    const File file(path);
    const auto stream = open(file);
    const auto& info = stream->info();
    const auto& layout = stream->layout();
    std::uint64_t from = 0;
    for (const auto key : stream->keyFrames()) {
      if (key <= target) {
        from = key;
      }
    }
    Pictures before(layout);
    Pictures now(layout);
    for (std::uint64_t frame = from; frame <= target; ++frame) {
      if (!stream->decode(frame, now.planes, frame == from ? nullptr : &before.planes)) {
        std::println(stderr, "frame {}: damaged", frame);
        return 1;
      }
      if (frame < target) {
        std::swap(before, now);
      }
    }
    FILE* stream_out = std::fopen(out, "wb");
    if (stream_out == nullptr) {
      return 2;
    }
    for (int plane = 0; plane < 3; ++plane) {
      const int width = plane == 0 ? info.width : (info.width + 1) / 2;
      const int height = plane == 0 ? info.height : (info.height + 1) / 2;
      for (int row = 0; row < height; ++row) {
        std::fwrite(
            now.planes.plane[static_cast<std::size_t>(plane)]
                + static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(layout.stride[static_cast<std::size_t>(plane)]),
            1, static_cast<std::size_t>(width), stream_out
        );
      }
    }
    std::fclose(stream_out);
    std::println("{} from key frame {}", target, from);
    return 0;
  }

  // The time to reach a picture the way an exact seek does: decode from the key frame before it.
  int seekTo(const char* path) {
    const File file(path);
    const auto stream = open(file);
    const auto& keys = stream->keyFrames();
    const std::uint64_t total = stream->info().frames;
    Pictures before(stream->layout());
    Pictures now(stream->layout());
    for (const std::uint64_t target : {total / 4, total / 2, total * 3 / 4}) {
      std::uint64_t from = 0;
      for (const auto key : keys) {
        if (key <= target) {
          from = key;
        }
      }
      const double cpuBegin = cpuSeconds();
      const auto begin = Clock::now();
      for (std::uint64_t frame = from; frame <= target; ++frame) {
        if (!stream->decode(frame, now.planes, frame == from ? nullptr : &before.planes)) {
          std::println(stderr, "frame {} is damaged", frame);
          return 1;
        }
        std::swap(before, now);
      }
      std::println(
          "picture {} from key frame {}: {} pictures, {:.0f} ms, {:.2f} s cpu", target, from, target - from + 1,
          milliseconds(begin), cpuSeconds() - cpuBegin
      );
    }
    return 0;
  }

  int poster(const char* path, int side) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      std::println(stderr, "cannot open {}", path);
      return 2;
    }
    for (int run = 0; run < 5; ++run) {
      const auto begin = Clock::now();
      const auto picture = luciole::media::bink2Poster(fd, side, [] { return false; });
      const double took = milliseconds(begin);
      if (!picture) {
        std::println(stderr, "{}", picture.error());
        return 1;
      }
      std::println("poster {}x{}: {:.1f} ms", picture->width, picture->height, took);
    }
    ::close(fd);
    return 0;
  }

  int seek(const char* path) {
    const File file(path);
    const auto stream = open(file);
    const auto& keys = stream->keyFrames();
    std::uint64_t from = 0;
    std::uint64_t span = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const std::uint64_t end = i + 1 < keys.size() ? keys[i + 1] : stream->info().frames;
      if (end - keys[i] > span) {
        span = end - keys[i];
        from = keys[i];
      }
    }
    Pictures before(stream->layout());
    Pictures now(stream->layout());
    const auto begin = Clock::now();
    for (std::uint64_t frame = from; frame < from + span; ++frame) {
      if (!stream->decode(frame, now.planes, frame == from ? nullptr : &before.planes)) {
        std::println(stderr, "frame {} is damaged", frame);
        return 1;
      }
      std::swap(before, now);
    }
    const double took = milliseconds(begin);
    std::println(
        "longest run: {} frames from key frame {}: {:.0f} ms, {:.2f} ms per frame", span, from, took,
        took / static_cast<double>(span)
    );
    return 0;
  }

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
} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::println(stderr, "usage: luciole-bink2-bench decode|play|seek FILE [...]");
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "decode") {
    return decode(argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 300);
  }
  if (mode == "sum") {
    return sum(argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 300);
  }
  if (mode == "adler") {
    return adlers(
        argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0,
        argc > 4 ? std::strtoull(argv[4], nullptr, 10) : ~0ULL >> 1
    );
  }
  if (mode == "scan") {
    return scan(
        argv[2], argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 0,
        argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 300
    );
  }
  if (mode == "dumprange" && argc > 5) {
    return dumpRange(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), argv[5]);
  }
  if (mode == "dump" && argc > 4) {
    return dump(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4]);
  }
  if (mode == "seekto") {
    return seekTo(argv[2]);
  }
  if (mode == "poster") {
    return poster(argv[2], argc > 3 ? std::atoi(argv[3]) : 256);
  }
  if (mode == "seek") {
    return seek(argv[2]);
  }
  if (mode == "play") {
    return play(argv[2], argc > 4 ? std::atoi(argv[3]) : 1280, argc > 4 ? std::atoi(argv[4]) : 720);
  }
  std::println(stderr, "unknown mode {}", mode);
  return 2;
}
