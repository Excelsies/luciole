#include "bink2_sample.h"
#include "media/bink2_stream.h"

#include <algorithm>
#include <cstdint>
#include <print>

// Pictures of one real sample against a second Bink 2 decoder (Librempeg's): the Adler-32 of the three planes of
// some pictures, from that decoder, as numbers. The sample is the owner's own file, found by its size and frame
// count; with any other file, or none, the test is skipped. The pictures are inter pictures after a key frame whose
// blocks carry DC values past 1023 (the frame flag 0x40000 widens the range to 2047) and whose luma vectors have a
// fraction both ways beside sharp edges; before the decoder followed that range and left its two passes unclipped,
// they came out with flat, stair-stepped patches.
namespace {
  using namespace test::bink2;

  constexpr std::uint64_t kSampleBytes = 538866256;
  constexpr std::uint64_t kSampleFrames = 6610;

  struct Known {
    std::uint64_t frame;
    std::uint32_t adler;
  };
  constexpr Known kKnown[] = {{478, 0xc64a694c}, {600, 0xce24e4e8}, {620, 0x0629faa0}};

  // The sum the other decoder's harness takes: Adler-32 from zero over the visible bytes of Y, U and V.
  std::uint32_t
  adler(const Picture& picture, const luciole::media::bink2::Info& info, const luciole::media::bink2::Layout& layout) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    for (std::size_t plane = 0; plane < 3; ++plane) {
      const int width = plane == 0 ? info.width : (info.width + 1) / 2;
      const int height = plane == 0 ? info.height : (info.height + 1) / 2;
      for (int row = 0; row < height; ++row) {
        const std::uint8_t* bytes = picture.planes.plane[plane] + static_cast<std::size_t>(row) * layout.stride[plane];
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
} // namespace

int main() {
  const auto sample = sampleFrom(kSampleVariable);
  if (!sample) {
    std::println("skipped: {} names no Bink 2 file", kSampleVariable);
    return kSkipExitCode;
  }
  const FileReader file(*sample);
  auto opened = luciole::media::bink2::Stream::open(file.reader(), file.size());
  TEST_CHECK(opened.has_value());
  auto& stream = **opened;
  if (file.size() != kSampleBytes || stream.info().frames != kSampleFrames) {
    std::println("skipped: the sample is not the file these pictures were taken from");
    return kSkipExitCode;
  }
  const auto& layout = stream.layout();
  Picture before(layout);
  Picture now(layout);
  // From the key frame before the first picture to the last one wanted.
  std::uint64_t from = 0;
  for (const auto key : stream.keyFrames()) {
    if (key <= kKnown[0].frame) {
      from = key;
    }
  }
  for (std::uint64_t frame = from; frame <= kKnown[2].frame; ++frame) {
    TEST_CHECK(stream.decode(frame, now.planes, frame == from ? nullptr : &before.planes).has_value());
    for (const auto& known : kKnown) {
      if (known.frame == frame) {
        TEST_CHECK(adler(now, stream.info(), layout) == known.adler);
      }
    }
    std::swap(before, now);
  }
  std::println("bink2-reference: all checks passed");
  return 0;
}
