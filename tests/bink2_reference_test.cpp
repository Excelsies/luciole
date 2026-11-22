#include "bink2_sample.h"
#include "media/bink2_stream.h"

#include <algorithm>
#include <cstdint>
#include <print>

// Three pictures (478, 600 and 620) of the Bink 2 file LUCIOLE_BINK2_SAMPLE names, decoded from the key frame before
// them, against the Adler-32 of their three planes from a second decoder. The file is recognized by its size and frame
// count (the numbers below); any other file, or none, skips the test.
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
  std::error_code sizeError;
  if (fs::file_size(*sample, sizeError) != kSampleBytes || sizeError) {
    std::println("skipped: {} names a file that is not the one these pictures were taken from", kSampleVariable);
    return kSkipExitCode;
  }
  const FileReader file(*sample);
  auto opened = luciole::media::bink2::Stream::open(file.reader(), file.size());
  if (!opened || (*opened)->info().frames != kSampleFrames) {
    std::println("skipped: the sample is not the file these pictures were taken from");
    return kSkipExitCode;
  }
  auto& stream = **opened;
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
