#include "bink2_sample.h"
#include "media/gstreamer_runtime.h"
#include "media/media_player.h"
#include "media_clips.h"
#include "test_check.h"

#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/base/gsttypefindhelper.h>
#include <print>
#include <vector>

// What needs no Bink 2 file: that GStreamer knows the format, and that files which are not Bink 2, or cannot be
// played, are refused with the reason, by the decoder, by the element and through the player.
namespace {
  namespace bink = test::bink2;
  using namespace test::bink2;
  using namespace test::media;
  using namespace std::chrono_literals;
  using luciole::AudioOutput;
  using luciole::MediaPlayer;
  using luciole::MediaState;
  using luciole::media::bink2::Stream;

  bool refusedBytes(const std::vector<std::uint8_t>& bytes, std::string_view words) {
    const auto opened = Stream::open(memoryReader(bytes), bytes.size());
    return !opened && opened.error().contains(words);
  }

  bool refused(const Container& container, std::string_view words) {
    const auto bytes = container.bytes();
    const auto opened = Stream::open(memoryReader(bytes), bytes.size());
    if (opened) {
      std::println(stderr, "opened a container that must be refused: {}", words);
      return false;
    }
    if (!opened.error().contains(words)) {
      std::println(stderr, "refusal '{}' does not say '{}'", opened.error(), words);
      return false;
    }
    return true;
  }

  Container oneFrame() {
    Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0xFF)};
    return container;
  }

  void refusesWhatIsNotABink2File() {
    const std::vector<std::uint8_t> text(200, 'a');
    TEST_CHECK(!Stream::open(memoryReader(text), text.size()));
    TEST_CHECK(Stream::open(memoryReader(text), text.size()).error().contains("not a Bink 2"));
    const std::vector<std::uint8_t> shorter(20, 'K');
    TEST_CHECK(Stream::open(memoryReader(shorter), shorter.size()).error().contains("too short"));
    auto bink1 = oneFrame();
    auto bytes = bink1.bytes();
    bytes[0] = 'B';
    bytes[1] = 'I';
    bytes[2] = 'K';
    TEST_CHECK(Stream::open(memoryReader(bytes), bytes.size()).error().contains("not a Bink 2"));
  }

  void refusesHeadersThatCannotBePlayed() {
    auto container = oneFrame();
    container.revision = 'z';
    TEST_CHECK(refused(container, "revision 'z'"));
    container = oneFrame();
    container.width = luciole::kMaxMediaFrameSide + 1;
    TEST_CHECK(refused(container, "at most 4096"));
    container = oneFrame();
    container.height = 0;
    TEST_CHECK(refused(container, "pixels"));
    container = oneFrame();
    container.width = 0xFFFFFFFF;
    container.height = 0xFFFFFFFF;
    TEST_CHECK(refused(container, "pixels"));
    container = oneFrame();
    container.rateDenominator = 0;
    TEST_CHECK(refused(container, "frame rate"));
    container = oneFrame();
    container.flags |= 0x00100000;
    TEST_CHECK(refused(container, "transparency"));
    container = oneFrame();
    container.tracks = 8;
    TEST_CHECK(refused(container, "8 sound tracks"));
    container = oneFrame();
    container.claimedFrames = 0;
    TEST_CHECK(refused(container, "0 frames"));
    container = oneFrame();
    container.claimedFrames = 5'000'000;
    TEST_CHECK(refused(container, "frames"));
    // A frame rate whose parts do not fit the caps' signed 32 bit fractions.
    container = oneFrame();
    container.rateNumerator = 0x80000000U;
    TEST_CHECK(refused(container, "frame rate"));
    container = oneFrame();
    container.rateDenominator = 0xFFFFFFFFU;
    TEST_CHECK(refused(container, "frame rate"));
  }

  // The sound a header may allow is bounded by what a track's own rate can lead the picture by, so no table claims
  // gigabytes of decoded sound; a file shorter than the index its header claims is refused before anything is made.
  void refusesSoundAndIndexesBeyondTheirBounds() {
    auto container = oneFrame();
    container.tracks = 1;
    container.maxDecoded = 0xFFFFFFFFU;
    TEST_CHECK(refused(container, "claims chunks"));
    container.maxDecoded = 48000U * 2 * 2 * 30 + 1;
    TEST_CHECK(refused(container, "claims chunks"));
    container.maxDecoded = 145920; // what a real file's first chunk decodes to
    container.frames = {std::vector<std::uint8_t>(64, 0)};
    const auto good = container.bytes();
    TEST_CHECK(!refusedBytes(good, "claims chunks"));
    // A tiny file that claims a million frames and seven tracks: refused at once, by size.
    auto tiny = oneFrame();
    tiny.claimedFrames = 1'000'000;
    tiny.tracks = 7;
    const auto begin = std::chrono::steady_clock::now();
    TEST_CHECK(refused(tiny, "shorter than the 1000000 frames"));
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1));
  }

  // A header that promises more frames than the file holds ends in the frame index, which is cut short.
  void refusesAnIndexThatIsCutShort() {
    auto container = oneFrame();
    container.claimedFrames = 40;
    TEST_CHECK(refused(container, "shorter"));
    auto bytes = oneFrame().bytes();
    bytes.resize(50);
    TEST_CHECK(Stream::open(memoryReader(bytes), bytes.size()).error().contains("shorter"));
  }

  void ratesAFrameThatIsNotPicturesAsDamaged() {
    const auto container = oneFrame();
    const auto bytes = container.bytes();
    auto opened = Stream::open(memoryReader(bytes), bytes.size());
    TEST_CHECK(opened.has_value());
    auto& stream = **opened;
    TEST_CHECK(stream.info().width == 64 && stream.info().height == 64 && stream.info().frames == 1);
    TEST_CHECK(stream.keyFrames() == std::vector<std::uint32_t>{0});
    Picture picture(stream.layout());
    const auto decoded = stream.decode(0, picture.planes, nullptr);
    TEST_CHECK(!decoded && decoded.error().contains("Frame 0"));
    TEST_CHECK(!stream.decode(1, picture.planes, nullptr));
  }

  void knowsTheFormat() {
    TEST_CHECK(luciole::media::startProblem().empty());
    GstElementFactory* factory = gst_element_factory_find("luciolebink2demux");
    TEST_CHECK(factory != nullptr);
    gst_object_unref(factory);
    const auto probe = [](std::vector<guint8> bytes) -> std::string {
      GstTypeFindProbability probability = GST_TYPE_FIND_NONE;
      GstCaps* caps = gst_type_find_helper_for_data(nullptr, bytes.data(), bytes.size(), &probability);
      if (caps == nullptr) {
        return {};
      }
      gchar* text = gst_caps_to_string(caps);
      std::string result = text;
      g_free(text);
      gst_caps_unref(caps);
      return probability == GST_TYPE_FIND_MAXIMUM ? result : "weak " + result;
    };
    // The magic alone is likely; a header the player accepts is certain.
    for (const guint8 revision : {'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n'}) {
      TEST_CHECK(probe({'K', 'B', '2', revision, 0, 0, 0, 0}) == "weak application/x-luciole-bink2");
      Container container = oneFrame();
      container.revision = static_cast<char>(revision);
      TEST_CHECK(probe(container.bytes()) == "application/x-luciole-bink2");
    }
    Container impossible = oneFrame();
    impossible.width = 0;
    TEST_CHECK(probe(impossible.bytes()) == "weak application/x-luciole-bink2");
    TEST_CHECK(probe({'K', 'B', '2', 'e', 0, 0, 0, 0}).empty());
    TEST_CHECK(probe({'K', 'B', '2', 'o', 0, 0, 0, 0}).empty());
    TEST_CHECK(probe({'B', 'I', 'K', 'i', 0, 0, 0, 0}).empty());
    TEST_CHECK(probe({'K', 'B', '1', 'n', 0, 0, 0, 0}).empty());
  }

  // A sparse file of four gigabytes that claims a million frames of two tracks and holds a valid index: opening it
  // reads the sound index frame by frame, which a stop must end promptly, not after all million.
  fs::path sparseFile(const fs::path& folder) {
    constexpr std::uint32_t kFrames = 1'000'000;
    constexpr std::uint32_t kStride = 4096;
    constexpr std::uint32_t kTracks = 2;
    const std::uint64_t indexAt = 44 + 4 + 12ULL * kTracks;
    const std::uint64_t firstFrame = indexAt + 4ULL * (kFrames + 1);
    std::vector<std::uint8_t> head(firstFrame, 0);
    const auto put = [&head](std::size_t at, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        head[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
      }
    };
    head[0] = 'K';
    head[1] = 'B';
    head[2] = '2';
    head[3] = 'n';
    put(4, static_cast<std::uint32_t>(firstFrame + static_cast<std::uint64_t>(kFrames) * kStride - 8));
    put(8, kFrames);
    put(20, 64);
    put(24, 64);
    put(28, 30);
    put(32, 1);
    put(36, 2);
    put(40, kTracks);
    for (std::uint32_t track = 0; track < kTracks; ++track) {
      put(48 + 4 * track, 4096);
      put(48 + 4 * kTracks + 4 * track, 0x70U << 24 | 48000);
    }
    for (std::uint32_t frame = 0; frame <= kFrames; ++frame) {
      put(indexAt + 4ULL * frame,
          static_cast<std::uint32_t>(firstFrame + static_cast<std::uint64_t>(frame) * kStride) | 1U);
    }
    const fs::path path = folder / "sparse.bik";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(::pwrite(fd, head.data(), head.size(), 0) == static_cast<ssize_t>(head.size()));
    TEST_CHECK(::ftruncate(fd, static_cast<off_t>(firstFrame + static_cast<std::uint64_t>(kFrames) * kStride)) == 0);
    ::close(fd);
    return path;
  }

  void openingAHostileFileCanBeStopped(const fs::path& folder) {
    const fs::path path = sparseFile(folder);
    FileReader file(path);
    // A stop that is already true ends the open at the first check, long before a million frames are read.
    const auto begin = std::chrono::steady_clock::now();
    const auto stopped = Stream::open(file.reader(), file.size(), [] { return true; });
    if (!stopped) {
      std::println(stderr, "sparse open: {}", stopped.error());
    }
    TEST_CHECK(!stopped && stopped.error().contains("stopped"));
    TEST_CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    // Through the element: going down while it opens returns within a bound.
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make("luciolebink2demux", nullptr);
    TEST_CHECK(pipeline != nullptr && source != nullptr && demux != nullptr);
    g_object_set(source, "location", path.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, demux, nullptr);
    TEST_CHECK(gst_element_link(source, demux) != FALSE);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    const auto down = std::chrono::steady_clock::now();
    TEST_CHECK(gst_element_set_state(pipeline, GST_STATE_NULL) == GST_STATE_CHANGE_SUCCESS);
    TEST_CHECK(std::chrono::steady_clock::now() - down < std::chrono::seconds(10));
    gst_object_unref(pipeline);
    std::println("a file that claims a million frames: the open stops at once");
  }

  // The first error a pipeline from the file through the element to a sink puts on the bus within 5 seconds.
  std::string pipelineError(const fs::path& path) {
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make("luciolebink2demux", nullptr);
    TEST_CHECK(pipeline != nullptr && source != nullptr && demux != nullptr);
    g_object_set(source, "location", path.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), source, demux, nullptr);
    TEST_CHECK(gst_element_link(source, demux) != FALSE);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
    std::string text;
    if (message != nullptr) {
      GError* problem = nullptr;
      gst_message_parse_error(message, &problem, nullptr);
      text = problem != nullptr ? problem->message : "";
      g_clear_error(&problem);
      gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return text;
  }

  void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  void reportsDamageAsAnError(const fs::path& folder) {
    bink::Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0xFF)};
    writeFile(folder / "damaged.bik", container.bytes());
    const auto text = pipelineError(folder / "damaged.bik");
    TEST_CHECK(text.contains("Frame 0"));
    std::println("a frame of garbage: {}", text);
    // A header that cannot be played ends the pipeline with the reason.
    container.width = 8000;
    writeFile(folder / "wide.bik", container.bytes());
    TEST_CHECK(pipelineError(folder / "wide.bik").contains("at most 4096"));
  }

  GstBusSyncReply dropEverything(GstBus* /*bus*/, GstMessage* /*message*/, gpointer /*data*/) { return GST_BUS_DROP; }

  // Decodebin drops an error that comes while it is still plugging the demuxer, which a file refused or damaged at its
  // start makes happen now and then. A bin that drops every message stands for it here: the reason must still reach
  // the bus of the pipeline.
  void anErrorReachesThePipelineBusThroughABinThatDropsMessages(
      const fs::path& folder, bool tooWide, std::string_view words
  ) {
    bink::Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0xFF)};
    if (tooWide) {
      container.width = 5000;
    }
    const fs::path path = folder / (tooWide ? "wide-bin.bik" : "damaged-bin.bik");
    writeFile(path, container.bytes());
    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* inner = gst_bin_new(nullptr);
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* demux = gst_element_factory_make("luciolebink2demux", nullptr);
    TEST_CHECK(pipeline != nullptr && inner != nullptr && source != nullptr && demux != nullptr);
    g_object_set(source, "location", path.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(inner), source, demux, nullptr);
    TEST_CHECK(gst_element_link(source, demux) != FALSE);
    gst_bin_add(GST_BIN(pipeline), inner);
    GstBus* innerBus = gst_element_get_bus(demux);
    TEST_CHECK(innerBus != nullptr);
    gst_bus_set_sync_handler(innerBus, dropEverything, nullptr, nullptr);
    gst_object_unref(innerBus);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
    std::string text;
    if (message != nullptr) {
      GError* problem = nullptr;
      gst_message_parse_error(message, &problem, nullptr);
      text = problem != nullptr ? problem->message : "";
      g_clear_error(&problem);
      gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    std::println("through a bin that drops messages: {}", text.empty() ? "no error" : text);
    TEST_CHECK(text.contains(words));
  }

  // The older Bink format is not played: no element takes it, and the player says it cannot be played.
  void aBink1FileEndsInAnError(Pump& pump, const fs::path& folder) {
    std::vector<std::uint8_t> bytes(4096, 0);
    bytes[0] = 'B';
    bytes[1] = 'I';
    bytes[2] = 'K';
    bytes[3] = 'i';
    writeFile(folder / "old.bik", bytes);
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open((folder / "old.bik").string()));
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
    TEST_CHECK(!player.error().empty());
    std::println("an old Bink file: {}", player.error());
  }

  void aDamagedHeaderEndsInAnError(Pump& pump, const fs::path& folder) {
    bink::Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0xFF)};
    const auto bytes = container.bytes();
    const fs::path path = folder / "damaged.bik";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    MediaPlayer player(AudioOutput::Silent);
    TEST_CHECK(player.open(path.string()));
    TEST_CHECK(pump.until([&] { return player.state() == MediaState::Error; }));
    TEST_CHECK(!player.error().empty());
    std::println("a damaged file: {}", player.error());
    // Too wide for the player.
    container.width = 5000;
    const auto wide = container.bytes();
    const fs::path widePath = folder / "wide.bik";
    std::ofstream(widePath, std::ios::binary)
        .write(reinterpret_cast<const char*>(wide.data()), static_cast<std::streamsize>(wide.size()));
    MediaPlayer other(AudioOutput::Silent);
    TEST_CHECK(other.open(widePath.string()));
    TEST_CHECK(pump.until([&] { return other.state() == MediaState::Error; }));
    TEST_CHECK(other.error().contains("4096"));
  }
} // namespace

int main() {
  Scratch scratch;
  Pump pump;
  const fs::path folder = scratch.path("x").parent_path();
  refusesWhatIsNotABink2File();
  refusesHeadersThatCannotBePlayed();
  refusesAnIndexThatIsCutShort();
  refusesSoundAndIndexesBeyondTheirBounds();
  ratesAFrameThatIsNotPicturesAsDamaged();
  knowsTheFormat();
  reportsDamageAsAnError(folder);
  openingAHostileFileCanBeStopped(folder);
  if (MediaPlayer::available()) {
    anErrorReachesThePipelineBusThroughABinThatDropsMessages(folder, true, "4096");
    anErrorReachesThePipelineBusThroughABinThatDropsMessages(folder, false, "Frame 0");
    aDamagedHeaderEndsInAnError(pump, folder);
    aBink1FileEndsInAnError(pump, folder);
  }
  std::println("bink2 refusal checks passed");
  return 0;
}
