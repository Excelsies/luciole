#include "bink2_sample.h"
#include "media/media_player.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <print>

extern "C" {
#include <bink2.h>
}

// The decoder behind the Bink 2 player: that decoding the slices of a frame on the pool gives, plane for plane, the
// pictures the library's own one-thread decoder gives, from the first frame and from a key frame in the middle of
// the file. It needs a real Bink 2 file: LUCIOLE_BINK2_SAMPLE names one, and without it the test exits with
// meson's skip code, 77. What needs no file is in bink2_refusal_test.
namespace {
  using namespace test::bink2;
  using luciole::media::bink2::Layout;
  using luciole::media::bink2::Stream;

  // Every byte of every plane, padding included, equal.
  bool samePlanes(const Layout& layout, const Picture& got, const std::uint8_t* const wanted[3]) {
    const std::uint8_t* const mine[3] = {got.planes.plane[0], got.planes.plane[1], got.planes.plane[2]};
    for (int plane = 0; plane < 3; ++plane) {
      if (std::memcmp(mine[plane], wanted[plane], layout.size[static_cast<std::size_t>(plane)]) != 0) {
        return false;
      }
    }
    return true;
  }

  // Decodes `count` frames from the key frame `first` both ways and compares each picture.
  void
  comparesWithTheLibrariesOwnDecoder(Stream& stream, const FileReader& file, std::uint64_t first, std::uint64_t count) {
    // The library's whole-frame decoder on a file of its own: its pictures are the reference.
    auto readFile = file.reader();
    const auto read = [](void* user, unsigned long offset, void* destination, unsigned long bytes) {
      return (*static_cast<luciole::media::bink2::Stream::Read*>(user))(offset, destination, bytes) ? 0 : -1;
    };
    bink2 reference = bink2_open_callbacks(read, &readFile, static_cast<unsigned long>(file.size()));
    TEST_CHECK(reference != nullptr);
    TEST_CHECK(bink2_enable_video(reference, 1) == 0);
    TEST_CHECK(bink2_seek_keyframe(reference, first) >= 0);
    const auto& layout = stream.layout();
    Picture before(layout);
    Picture now(layout);
    for (std::uint64_t frame = first; frame < first + count; ++frame) {
      if (frame > first) {
        TEST_CHECK(bink2_next(reference) >= 0);
      }
      const std::uint8_t* const wanted[3] = {
          bink2_get_plane(reference, BINK2_PLANE_Y), bink2_get_plane(reference, BINK2_PLANE_U),
          bink2_get_plane(reference, BINK2_PLANE_V)
      };
      TEST_CHECK(wanted[0] != nullptr && wanted[1] != nullptr && wanted[2] != nullptr);
      const auto decoded = stream.decode(frame, now.planes, frame == first ? nullptr : &before.planes);
      if (!decoded) {
        std::println(stderr, "frame {}: {}", frame, decoded.error());
      }
      TEST_CHECK(decoded.has_value());
      if (!samePlanes(layout, now, wanted)) {
        std::println(stderr, "frame {} differs from the library's own decode", frame);
        TEST_CHECK(false);
      }
      std::swap(before, now);
    }
    bink2_close(reference);
  }

  void poolDecodeEqualsTheLibrariesDecode(const fs::path& path) {
    FileReader file(path);
    auto opened = Stream::open(file.reader(), file.size());
    if (!opened) {
      std::println(stderr, "open: {}", opened.error());
    }
    TEST_CHECK(opened.has_value());
    auto& stream = **opened;
    const auto& keys = stream.keyFrames();
    TEST_CHECK(!keys.empty() && keys.front() == 0);
    // The start of the file, and the run after a key frame in the middle of the file.
    const std::uint64_t middle = keys[keys.size() / 2];
    comparesWithTheLibrariesOwnDecoder(stream, file, 0, std::min<std::uint64_t>(stream.info().frames, 40));
    if (middle != 0) {
      comparesWithTheLibrariesOwnDecoder(
          stream, file, middle, std::min<std::uint64_t>(stream.info().frames - middle, 40)
      );
    }
    std::println(
        "decoded {}x{}, {} frames, {} key frames: pool equals library", stream.info().width, stream.info().height,
        stream.info().frames, keys.size()
    );
  }

  // What a decode costs per frame with the pool, over the start of the file.
  void measuresADecode(const fs::path& path) {
    FileReader file(path);
    auto opened = Stream::open(file.reader(), file.size());
    TEST_CHECK(opened.has_value());
    auto& stream = **opened;
    const auto frames = std::min<std::uint64_t>(stream.info().frames, 120);
    Picture before(stream.layout());
    Picture now(stream.layout());
    const auto begin = std::chrono::steady_clock::now();
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      TEST_CHECK(stream.decode(frame, now.planes, frame == 0 ? nullptr : &before.planes).has_value());
      std::swap(before, now);
    }
    const auto took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::println("{:.2f} ms per frame over {} frames", took / static_cast<double>(frames), frames);
  }
  // A sound chunk decodes to the bytes it states and no more, however much data follows its claim: a small file must
  // not make the decoder keep far more sound than it holds. Chunks that claim more than their bits hold, or more
  // than the track allows, fail.
  void soundIsBoundedByItsClaim(const fs::path& sample) {
    constexpr std::uint32_t kAllowed = 145920;
    const auto decode = [&](std::uint32_t claim, std::uint32_t chunkBytes) -> std::optional<std::size_t> {
      const auto bytes = withSoundChunk(sample, claim, chunkBytes, kAllowed).bytes();
      auto opened = Stream::open(memoryReader(bytes), bytes.size());
      // A claim above what the track allows is refused when the index is read, at open.
      if (!opened || !(*opened)->decodeSound(0)) {
        return std::nullopt;
      }
      return (*opened)->audio(0).size();
    };
    // Eight megabytes of zero bits behind a claim of one block: one block comes out.
    TEST_CHECK(decode(7680, 8U << 20) == std::optional<std::size_t>(7680));
    TEST_CHECK(decode(1280, 4096) == std::optional<std::size_t>(1280));
    TEST_CHECK(!decode(kAllowed, 40).has_value());
    TEST_CHECK(!decode(kAllowed + 1, 1U << 20).has_value());
    TEST_CHECK(!decode(0, 4096).has_value());
    std::println("sound chunks are held to their claim");
  }
} // namespace

int main() {
  const auto sample = sampleFrom(kSampleVariable);
  needSample(sample, kSampleVariable);
  soundIsBoundedByItsClaim(*sample);
  poolDecodeEqualsTheLibrariesDecode(*sample);
  measuresADecode(*sample);
  std::println("bink2 stream checks passed");
  return 0;
}
