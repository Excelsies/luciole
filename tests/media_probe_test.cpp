#include "media/media_probe.h"
#include "media/pipeline_thread.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

// The facts of a file read from its container before anything plays: playing time and picture size.
namespace {
  using luciole::MediaFacts;
  using luciole::MediaProbe;
  using namespace test::media;

  // One probe and what its callback was given; `calls` counts the callbacks.
  struct Probed {
    MediaProbe probe;
    std::optional<std::optional<MediaFacts>> result;
    int calls = 0;

    void start(const fs::path& path, std::chrono::milliseconds limit = MediaProbe::kTimeLimit) {
      result.reset();
      probe.start(
          path.string(),
          [this](std::optional<MediaFacts> facts) {
            result = facts;
            ++calls;
          },
          limit
      );
    }
    bool finished(Pump& pump) {
      return pump.until([this] { return result.has_value(); });
    }
  };

  void readsAVideoWithSound(Pump& pump, const fs::path& clip) {
    Probed probed;
    probed.start(clip);
    TEST_CHECK(probed.probe.busy());
    TEST_CHECK(probed.finished(pump));
    TEST_CHECK(!probed.probe.busy());
    TEST_CHECK(probed.calls == 1);
    TEST_CHECK(probed.result->has_value());
    const auto& facts = **probed.result;
    TEST_NEAR(facts.seconds, 3.0, 0.05);
    TEST_CHECK(facts.video && facts.audio);
    TEST_CHECK(facts.width == 160 && facts.height == 120);
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }

  void readsSoundOnly(Pump& pump, const fs::path& tone) {
    Probed probed;
    probed.start(tone);
    TEST_CHECK(probed.finished(pump));
    TEST_CHECK(probed.result->has_value());
    const auto& facts = **probed.result;
    TEST_NEAR(facts.seconds, 2.0, 0.05);
    TEST_CHECK(!facts.video && facts.audio);
    TEST_CHECK(facts.width == 0 && facts.height == 0);
  }

  // Streams that a parser exposes before their caps are complete: MPEG-4 video with MP3 sound in MP4, FLAC and MP3.
  void readsStreamsThroughAParser(Pump& pump, const Scratch& scratch) {
    const auto mp4 = scratch.path("parsed.mp4");
    const auto flac = scratch.path("parsed.flac");
    const auto mp3 = scratch.path("parsed.mp3");
    TEST_CHECK(writeMp4(mp4));
    TEST_CHECK(writeSound(flac, "flacenc"));
    TEST_CHECK(writeSound(mp3, "lamemp3enc"));
    const auto read = [&pump](const fs::path& path) {
      Probed probed;
      probed.start(path);
      TEST_CHECK(probed.finished(pump));
      TEST_CHECK(probed.result->has_value());
      const auto facts = probed.result->value_or(MediaFacts{});
      std::println(
          "{}: {:.2f} s, {}x{}, video {}, audio {}", path.filename().string(), facts.seconds, facts.width, facts.height,
          facts.video, facts.audio
      );
      return facts;
    };
    const auto movie = read(mp4);
    TEST_CHECK(movie.video && movie.audio);
    TEST_CHECK(movie.width == 96 && movie.height == 64);
    TEST_NEAR(movie.seconds, 2.0, 0.1);
    for (const auto& path : {flac, mp3}) {
      const auto sound = read(path);
      TEST_CHECK(sound.audio && !sound.video);
      TEST_CHECK(sound.width == 0 && sound.height == 0);
      TEST_NEAR(sound.seconds, 2.0, 0.1);
    }
    TEST_CHECK(pump.until([&] { return !holdsOpen(mp4) && !holdsOpen(flac) && !holdsOpen(mp3); }));
  }

  // Counts the opens of one file; closes are watched too, since inotify merges equal events that follow each
  // other unread.
  class OpenCounter {
  public:
    explicit OpenCounter(const fs::path& path) : m_fd(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
      TEST_CHECK(m_fd >= 0);
      TEST_CHECK(::inotify_add_watch(m_fd, path.c_str(), IN_OPEN | IN_CLOSE) >= 0);
    }
    ~OpenCounter() { ::close(m_fd); }
    OpenCounter(const OpenCounter&) = delete;
    OpenCounter& operator=(const OpenCounter&) = delete;

    // The opens since the last call. A watch on a file reports events of a fixed size, without a name.
    long opens() {
      std::array<inotify_event, 256> events{};
      const auto length = ::read(m_fd, events.data(), sizeof(events));
      const auto count = length > 0 ? std::min(static_cast<std::size_t>(length) / sizeof(inotify_event), events.size())
                                    : std::size_t{0};
      return std::count_if(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count), [](const auto& event) {
        return (event.mask & IN_OPEN) != 0;
      });
    }

  private:
    int m_fd;
  };

  // A read dropped while it waits behind another never opens its file.
  void skipsReadsDroppedInTheQueue(Pump& pump, const fs::path& clip) {
    // The probe thread waits on this pipe until the reads below are queued and dropped; the reads queued before
    // it have then finished, so the watch sees only these.
    std::array<int, 2> gate{-1, -1};
    TEST_CHECK(::pipe2(gate.data(), O_CLOEXEC) == 0);
    std::promise<void> waiting;
    luciole::media::runOnProbeThread([&waiting, readEnd = gate[0]] {
      waiting.set_value();
      char byte = 0;
      (void)::read(readEnd, &byte, 1);
    });
    TEST_CHECK(waiting.get_future().wait_for(kWait) == std::future_status::ready);
    OpenCounter counter(clip);
    constexpr int kDropped = 20;
    int calls = 0;
    for (int i = 0; i < kDropped; ++i) {
      MediaProbe dropped;
      dropped.start(clip.string(), [&calls](std::optional<MediaFacts> /*facts*/) { ++calls; });
      dropped.cancel();
    }
    // One read that is kept marks the end of the queue: once it called back, every read before it was handled.
    Probed kept;
    kept.start(clip);
    TEST_CHECK(::write(gate[1], "x", 1) == 1);
    TEST_CHECK(kept.finished(pump));
    TEST_CHECK(calls == 0);
    const long opens = counter.opens();
    // The same kept read alone opens the clip as often.
    kept.start(clip);
    TEST_CHECK(kept.finished(pump));
    const long alone = counter.opens();
    std::println(
        "opens of the clip: {} for {} dropped reads and one kept, {} for one kept alone", opens, kDropped, alone
    );
    TEST_CHECK(alone > 0);
    TEST_CHECK(opens == alone);
    for (const int fd : gate) {
      ::close(fd);
    }
  }

  // What is not media, not there or not a regular file gives no facts, and a FIFO is never opened.
  void givesNothingForWhatItCannotRead(Pump& pump, const fs::path& broken) {
    const auto fifo = broken.parent_path() / "trap.webm";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    for (const auto& path : {broken, broken.parent_path() / "missing.mkv", fifo, broken.parent_path()}) {
      Probed probed;
      probed.start(path);
      TEST_CHECK(probed.finished(pump));
      TEST_CHECK(!probed.result->has_value());
    }
    Probed relative;
    relative.probe.start("clip.avi", [&relative](std::optional<MediaFacts> facts) { relative.result = facts; });
    TEST_CHECK(relative.finished(pump));
    TEST_CHECK(!relative.result->has_value());
  }

  // A playlist on disk is read without a connection to the address it names.
  void neverReachesTheNetwork(Pump& pump, const fs::path& playlist) {
    const Listener server;
    writeBytes(
        playlist,
        std::format(
            "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nhttp://127.0.0.1:{}/a.ts\n"
            "#EXT-X-ENDLIST\n",
            server.port()
        )
    );
    Probed probed;
    probed.start(playlist);
    TEST_CHECK(probed.finished(pump));
    TEST_CHECK(!probed.result->has_value());
    pump.run(std::chrono::milliseconds(500)); // time for a request that slipped through to arrive
    TEST_CHECK(!server.connected());
  }

  // A read that runs past its time limit ends with no facts.
  void stopsAtItsTimeLimit(Pump& pump, const fs::path& clip) {
    Probed probed;
    probed.start(clip, std::chrono::milliseconds(0));
    TEST_CHECK(probed.finished(pump));
    TEST_CHECK(!probed.result->has_value());
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }

  // A cancelled, replaced or destroyed probe never calls back for the read it dropped.
  void cancels(Pump& pump, const fs::path& clip, const fs::path& tone) {
    Probed probed;
    probed.start(clip);
    probed.probe.cancel();
    TEST_CHECK(!probed.probe.busy());
    pump.run(std::chrono::milliseconds(500));
    TEST_CHECK(probed.calls == 0);

    probed.start(clip);
    probed.start(tone);
    TEST_CHECK(probed.finished(pump));
    pump.run(std::chrono::milliseconds(300));
    TEST_CHECK(probed.calls == 1);
    TEST_CHECK(probed.result->has_value() && !(*probed.result)->video);

    int late = 0;
    {
      MediaProbe dropped;
      dropped.start(clip.string(), [&late](std::optional<MediaFacts> /*facts*/) { ++late; });
    }
    pump.run(std::chrono::milliseconds(500));
    TEST_CHECK(late == 0);
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }
} // namespace

int main() {
  if (!MediaProbe::available()) {
    MediaProbe probe;
    bool called = false;
    probe.start("/clip.avi", [&called](std::optional<MediaFacts> /*facts*/) { called = true; });
    TEST_CHECK(!probe.busy() && !called);
    std::println("media-probe: playback not built");
    return 0;
  }
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "parsebin",
           "fakesink"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  const auto tone = scratch.path("tone.wav");
  const auto broken = scratch.path("broken.avi");
  TEST_CHECK(writeClip(clip, {}));
  writeTone(tone, 2);
  writeBytes(broken, "this is not a movie at all, only a few words of text");
  Pump pump;
  readsAVideoWithSound(pump, clip);
  readsSoundOnly(pump, tone);
  givesNothingForWhatItCannotRead(pump, broken);
  if (missingElements({"hlsdemux2"}).empty()) {
    neverReachesTheNetwork(pump, scratch.path("stream.m3u8"));
  } else {
    std::println("network refusal not checked: no hlsdemux2");
  }
  stopsAtItsTimeLimit(pump, clip);
  cancels(pump, clip, tone);
  skipsReadsDroppedInTheQueue(pump, clip);
  if (const auto lacking = missingElements(kParsedClipElements); lacking.empty()) {
    readsStreamsThroughAParser(pump, scratch);
  } else {
    std::println("parsed streams not checked: GStreamer lacks {}", lacking);
  }
  std::println("media-probe: all checks passed");
  return 0;
}
