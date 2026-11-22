#include "bink2_sample.h"
#include "media/bink2_stream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <print>
#include <random>
#include <vector>

extern "C" {
#include <bink2_idct.h>
}

// The inverse transform of the Bink 2 decoder: that its fast paths give, byte for byte, what the plain C path gives.
// Blocks made to be hard (the extreme values of every coefficient, sparse blocks, blocks that overflow the 16 bit
// steps) are put and added through both, and, when LUCIOLE_BINK2_SAMPLE names a real file, the pictures of its first
// frames are decoded through both. Without the file only the blocks run.
namespace {
  using namespace test::bink2;

  using Block = std::array<std::int16_t, 64>;
  constexpr int kStride = 24;
  constexpr std::array<std::int16_t, 12> kExtremes{-32768, 32767, -32767, 0, 1, -1, 63, -64, 64, 1023, -1024, 255};

  // The block put and added to a destination of random bytes through the fast path and through the plain C path.
  void sameResult(const Block& block, std::mt19937& random) {
    std::array<std::uint8_t, kStride * 8> start{};
    for (auto& byte : start) {
      byte = static_cast<std::uint8_t>(random());
    }
    auto fastPut = start;
    auto plainPut = start;
    auto fastAdd = start;
    auto plainAdd = start;
    bink2_idct_put(fastPut.data(), kStride, block.data());
    bink2_idct_put_plain(plainPut.data(), kStride, block.data());
    bink2_idct_add(fastAdd.data(), kStride, block.data());
    bink2_idct_add_plain(plainAdd.data(), kStride, block.data());
    TEST_CHECK(fastPut == plainPut);
    TEST_CHECK(fastAdd == plainAdd);
  }

  void extremeBlocks() {
    std::mt19937 random(7);
    Block block{};
    sameResult(block, random); // all zero
    // One coefficient at a time, at each extreme: the DC alone, then every other place.
    for (int place = 0; place < 64; ++place) {
      for (const auto value : kExtremes) {
        block = {};
        block[static_cast<std::size_t>(place)] = value;
        sameResult(block, random);
      }
    }
    // A DC with one AC, and every pair of extremes in the first row and column.
    for (const auto dc : kExtremes) {
      for (int place = 1; place < 64; ++place) {
        for (const auto value : {std::int16_t{-32768}, std::int16_t{32767}, std::int16_t{5}}) {
          block = {};
          block[0] = dc;
          block[static_cast<std::size_t>(place)] = value;
          sameResult(block, random);
        }
      }
    }
    // Every coefficient at one extreme, in each combination of two.
    for (const auto a : kExtremes) {
      for (const auto b : kExtremes) {
        for (int i = 0; i < 64; ++i) {
          block[static_cast<std::size_t>(i)] = (i & 1) != 0 ? a : b;
        }
        sameResult(block, random);
        for (int i = 0; i < 64; ++i) {
          block[static_cast<std::size_t>(i)] = i < 32 ? a : b;
        }
        sameResult(block, random);
      }
    }
  }

  void randomBlocks() {
    std::mt19937 random(11);
    Block block{};
    for (int round = 0; round < 40000; ++round) {
      // Full range, small, and sparse blocks in turn.
      const int kind = round % 4;
      for (auto& value : block) {
        const auto bits = random();
        switch (kind) {
        case 0:
          value = static_cast<std::int16_t>(bits);
          break;
        case 1:
          value = static_cast<std::int16_t>(static_cast<int>(bits % 513) - 256);
          break;
        case 2:
          value = (bits & 7U) == 0 ? static_cast<std::int16_t>(static_cast<int>((bits >> 3U) % 4097) - 2048) : 0;
          break;
        default:
          value = (bits & 31U) == 0 ? static_cast<std::int16_t>(bits >> 8U) : 0;
          break;
        }
      }
      if (round % 8 >= 4) {
        std::fill(block.begin() + 1, block.end(), std::int16_t{0}); // DC alone, as most blocks are
      }
      sameResult(block, random);
    }
  }

  // The pictures of the first frames of the sample, decoded with the fast paths and with the plain C path: every
  // byte of every plane, padding included, equal.
  void samplePictures() {
    const auto sample = sampleFrom(kSampleVariable);
    if (!sample) {
      std::println("no sample: {} names no Bink 2 file; the pictures are not compared", kSampleVariable);
      return;
    }
    const FileReader file(*sample);
    auto opened = luciole::media::bink2::Stream::open(file.reader(), file.size());
    TEST_CHECK(opened.has_value());
    auto& stream = **opened;
    const auto& layout = stream.layout();
    const std::uint64_t frames = std::min<std::uint64_t>(stream.info().frames, 24);
    Picture fastBefore(layout);
    Picture fastNow(layout);
    Picture plainBefore(layout);
    Picture plainNow(layout);
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      bink2_idct_use_plain(0);
      TEST_CHECK(stream.decode(frame, fastNow.planes, frame == 0 ? nullptr : &fastBefore.planes).has_value());
      bink2_idct_use_plain(1);
      TEST_CHECK(stream.decode(frame, plainNow.planes, frame == 0 ? nullptr : &plainBefore.planes).has_value());
      for (std::size_t plane = 0; plane < 3; ++plane) {
        TEST_CHECK(std::memcmp(fastNow.planes.plane[plane], plainNow.planes.plane[plane], layout.size[plane]) == 0);
      }
      std::swap(fastBefore, fastNow);
      std::swap(plainBefore, plainNow);
    }
    bink2_idct_use_plain(0);
  }
} // namespace

int main() {
  extremeBlocks();
  randomBlocks();
  samplePictures();
  std::println("bink2-idct: all checks passed");
  return 0;
}
