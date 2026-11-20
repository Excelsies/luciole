#include "bink2_worker.h"
#include "media/gstreamer_runtime.h"
#include "media_clips.h"
#include "test_check.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <gst/gst.h>
#include <print>
#include <random>
#include <string_view>
#include <vector>

// Damaged Bink 2 files must end in an error with a reason, or play on, and never crash, hang or take unbounded
// memory. The files are made from the start of a real Bink 2 file, which LUCIOLE_BINK2_SAMPLE names (without it the
// test exits with meson's skip code, 77), in a private folder: bytes
// changed in the frames and in the header, cuts, and headers that claim absurd sizes. Each goes through the decoder
// and through a pipeline. LUCIOLE_BINK2_DAMAGE_SEEDS (default 6) sets how many seeds each kind of change gets;
// LUCIOLE_BINK2_DAMAGE_DIR names a folder of further .bik files to run the same way.
namespace {
  namespace bink = test::bink2;
  using namespace test::media;
  using namespace std::chrono_literals;
  using Bytes = std::vector<std::uint8_t>;

  constexpr int kFrames = 8;
  constexpr auto kLimit = 30s;

  std::uint32_t le32(const Bytes& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at])
        | static_cast<std::uint32_t>(bytes[at + 1]) << 8U
        | static_cast<std::uint32_t>(bytes[at + 2]) << 16U
        | static_cast<std::uint32_t>(bytes[at + 3]) << 24U;
  }

  void put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
      bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  Bytes readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  struct Outcome {
    int decoded = 0;
    int refused = 0; // frames or files the decoder refused with a reason
  };

  // The decoder on its own: open, then every frame in order from the key frame; each failure carries a reason.
  Outcome throughTheDecoder(const fs::path& path) {
    Outcome outcome;
    bink::FileReader file(path);
    auto opened = luciole::media::bink2::Stream::open(file.reader(), file.size());
    if (!opened) {
      TEST_CHECK(!opened.error().empty());
      ++outcome.refused;
      return outcome;
    }
    auto& stream = **opened;
    bink::Picture before(stream.layout());
    bink::Picture now(stream.layout());
    bool haveBefore = false;
    for (std::uint64_t frame = 0; frame < std::min<std::uint64_t>(stream.info().frames, kFrames); ++frame) {
      const auto done = stream.decode(frame, now.planes, haveBefore ? &before.planes : nullptr);
      if (done) {
        ++outcome.decoded;
        std::swap(before, now);
        haveBefore = true;
      } else {
        TEST_CHECK(!done.error().empty());
        ++outcome.refused;
        haveBefore = false;
      }
    }
    return outcome;
  }

  // The element in a pipeline: it ends at the end of the stream or on an error that names its cause, within a limit.
  void throughAPipeline(const fs::path& path) {
    const auto description = std::format(
        "filesrc location=\"{}\" ! luciolebink2demux name=d d.video_0 ! queue ! fakesink sync=false", path.string()
    );
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    TEST_CHECK(pipeline != nullptr && error == nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* done = gst_bus_timed_pop_filtered(
        bus, static_cast<GstClockTime>(std::chrono::duration_cast<std::chrono::nanoseconds>(kLimit).count()),
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)
    );
    TEST_CHECK(done != nullptr);
    if (GST_MESSAGE_TYPE(done) == GST_MESSAGE_ERROR) {
      GError* problem = nullptr;
      gst_message_parse_error(done, &problem, nullptr);
      TEST_CHECK(problem != nullptr && problem->message != nullptr && *problem->message != '\0');
      g_clear_error(&problem);
    }
    gst_message_unref(done);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }

  void run(const fs::path& folder, const std::string& name, const Bytes& bytes, Outcome& total) {
    const fs::path path = folder / (name + ".bik");
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto outcome = throughTheDecoder(path);
    throughAPipeline(path);
    // The poster helper ends in a picture or in a reason it wrote; a crash, a hang or a kill leaves neither.
    const auto helper = bink::runWorker(folder, path, 128);
    TEST_CHECK(helper.started || helper.reported);
    total.decoded += outcome.decoded;
    total.refused += outcome.refused;
    std::error_code ignored;
    fs::remove(path, ignored);
  }

  int seeds() {
    const char* value = std::getenv("LUCIOLE_BINK2_DAMAGE_SEEDS");
    int count = 6;
    if (value != nullptr) {
      const std::string_view text = value;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        std::println(stderr, "LUCIOLE_BINK2_DAMAGE_SEEDS is not a number: {}", text);
        TEST_CHECK(false);
        return 1;
      }
    }
    return std::clamp(count, 1, 200);
  }

  void damage(const fs::path& sample, const fs::path& folder) {
    const Bytes whole = readAll(sample);
    TEST_CHECK(whole.size() > 4096);
    const std::uint32_t tracks = le32(whole, 40);
    const std::size_t indexAt = 44 + 4 + 12UL * tracks;
    const std::size_t firstFrame = le32(whole, indexAt) & ~1U;
    const std::size_t end = le32(whole, indexAt + 4UL * kFrames) & ~1U;
    TEST_CHECK(firstFrame < end && end < whole.size());
    const Bytes start(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(end));
    Outcome total;
    int files = 0;
    for (int seed = 1; seed <= seeds(); ++seed) {
      std::mt19937 random(static_cast<std::uint32_t>(seed));
      Bytes data = start;
      for (int i = 0; i < 30; ++i) {
        data[firstFrame + random() % (data.size() - firstFrame)] = static_cast<std::uint8_t>(random());
      }
      run(folder, std::format("data{}", seed), data, total);
      Bytes index = start;
      for (int i = 0; i < 4; ++i) {
        index[4 + random() % (firstFrame - 4)] = static_cast<std::uint8_t>(random());
      }
      run(folder, std::format("index{}", seed), index, total);
      files += 2;
    }
    // Cuts at tenths of the start, which end inside a frame.
    for (int tenth = 1; tenth < 10; ++tenth) {
      run(folder, std::format("cut{}", tenth),
          Bytes(start.begin(), start.begin() + static_cast<std::ptrdiff_t>(start.size() * tenth / 10)), total);
      ++files;
    }
    // Headers that claim sizes the player must refuse before it allocates for them, over the frames of the file.
    struct Claim {
      const char* name;
      std::size_t at;
      std::uint32_t value;
    };
    for (const Claim& claim :
         {Claim{"wide", 20, 7680}, Claim{"tall", 24, 4800}, Claim{"huge", 20, 0xFFFFFFFF},
          Claim{"frames", 8, 1'000'000}, Claim{"manyframes", 8, 0xFFFFFFFF}, Claim{"tracks", 40, 7},
          Claim{"rate", 28, 0}, Claim{"alpha", 36, 0x00140013}, Claim{"small", 20, 32}, Claim{"odd", 20, 3841}}) {
      Bytes claimed = start;
      put32(claimed, claim.at, claim.value);
      run(folder, claim.name, claimed, total);
      ++files;
    }
    // Sound chunks that claim little behind a great deal of data, and a table that allows gigabytes.
    run(folder, "soundbomb", bink::withSoundChunk(sample, 7680, 8U << 20, 145920).bytes(), total);
    run(folder, "soundtable", bink::withSoundChunk(sample, 7680, 8U << 20, 0xFFFFFFFFU).bytes(), total);
    files += 2;
    // A header alone.
    run(folder, "headeronly", Bytes(whole.begin(), whole.begin() + 4096), total);
    ++files;
    run(folder, "nothing", Bytes(), total);
    ++files;
    std::println("{} damaged files: {} frames decoded, {} refused with a reason", files, total.decoded, total.refused);
    TEST_CHECK(total.refused > 0);
  }

  void furtherFiles(const fs::path& folder) {
    const char* named = std::getenv("LUCIOLE_BINK2_DAMAGE_DIR");
    if (named == nullptr || *named == '\0') {
      return;
    }
    Outcome total;
    int files = 0;
    for (const auto& entry : fs::directory_iterator(named)) {
      if (entry.is_regular_file() && entry.path().extension() == ".bik") {
        run(folder, std::format("further{}", files), readAll(entry.path()), total);
        ++files;
      }
    }
    std::println("{} further files: {} frames decoded, {} refused with a reason", files, total.decoded, total.refused);
  }
} // namespace

int main(int argc, char** argv) {
  if (const int code = bink::dispatchBink2Worker(argc, argv); code >= 0) {
    return code;
  }
  Scratch scratch;
  TEST_CHECK(luciole::media::startProblem().empty());
  const auto sample = bink::sampleFrom(bink::kSampleVariable);
  const auto& path = bink::needSample(sample, bink::kSampleVariable);
  const fs::path folder = scratch.path("damage").parent_path();
  damage(path, folder);
  furtherFiles(folder);
  std::println("bink2 damage checks passed");
  return 0;
}
