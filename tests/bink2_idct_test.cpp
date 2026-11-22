#include "bink2_mc_reference.h"
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
#include <bink2_dsp.h>
#include <bink2_idct.h>
}

// The inverse transform and the motion compensation of the Bink 2 decoder: that its fast paths give, byte for byte,
// what the plain C path gives. Blocks made to be hard (the extreme values of every coefficient, sparse blocks, blocks
// that overflow the 16 bit steps) are put and added through both, and, when LUCIOLE_BINK2_SAMPLE names a real file, the
// pictures of its first frames are decoded through both. Without the file only the blocks run.
namespace {
  using namespace test::bink2;
  namespace reference = test::bink2::reference;

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
    // A block known to hold only its first coefficient has its own entry.
    if (std::all_of(block.begin() + 1, block.end(), [](std::int16_t value) { return value == 0; })) {
      auto dcPut = start;
      auto dcAdd = start;
      bink2_idct_put_dc(dcPut.data(), kStride, block[0]);
      bink2_idct_add_dc(dcAdd.data(), kStride, block[0]);
      TEST_CHECK(dcPut == plainPut);
      TEST_CHECK(dcAdd == plainAdd);
    }
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

  // Motion compensation on random pictures, for every vector fraction and at the edges, against the reference.
  void motionCompensation() {
    std::mt19937 random(13);
    constexpr int kWidth = 96;
    constexpr int kHeight = 80;
    constexpr int kPitch = 112;
    // The decoder's planes are padded to whole 32 rows, and a vector reads up to 18 rows past a block.
    constexpr int kPaddedHeight = (kHeight + 31) & ~31;
    std::vector<std::uint8_t> source(static_cast<std::size_t>(kPitch) * kPaddedHeight);
    for (auto& byte : source) {
      byte = static_cast<std::uint8_t>(random());
    }
    for (int round = 0; round < 40000; ++round) {
      if (round % 4 == 1) { // flat and extreme pictures saturate the filters
        for (auto& byte : source) {
          byte = (random() & 1U) != 0 ? 255 : 0;
        }
      }
      const bool luma = round % 2 == 0;
      const int block = luma ? 16 : 8;
      const int x = static_cast<int>(random() % (kWidth / block)) * block;
      const int y = static_cast<int>(random() % (kHeight / block)) * block;
      const int range = luma ? 80 : 160;
      const int mvx = static_cast<int>(random() % (2 * range)) - range;
      const int mvy = static_cast<int>(random() % (2 * range)) - range;
      std::array<std::uint8_t, 24 * 24> fast{};
      std::array<std::uint8_t, 24 * 24> plain{};
      if (luma) {
        bink2_mc_luma(fast.data(), 24, source.data(), kPitch, x, y, kWidth, kHeight, mvx, mvy);
        reference::ref_mc_luma(plain.data(), 24, source.data(), kPitch, x, y, kWidth, kHeight, mvx, mvy);
      } else {
        bink2_mc_chroma(fast.data(), 24, source.data(), kPitch, x, y, kWidth, kHeight, mvx, mvy);
        reference::ref_mc_chroma(plain.data(), 24, source.data(), kPitch, x, y, kWidth, kHeight, mvx, mvy);
      }
      TEST_CHECK(fast == plain);
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
  // The first pictures of the sample read through bit flips made in memory (the file is never changed): the fast
  // and the plain paths must end each picture the same way, with the same error or the same bytes in every plane.
  // The flips fall in the packets of the first frames, where the coefficients are; the container is left whole.
  void damagedPictures() {
    const auto sample = sampleFrom(kSampleVariable);
    if (!sample) {
      std::println("no sample: {} names no Bink 2 file; damaged pictures are not compared", kSampleVariable);
      return;
    }
    const FileReader file(*sample);
    constexpr std::uint32_t kFrames = 4;
    std::vector<std::uint8_t> head(4096);
    TEST_CHECK(file.reader()(0, head.data(), head.size()));
    const std::size_t index = 44 + 4 + 12UL * le32(head, 40);
    const std::uint64_t first = le32(head, index) & ~1U;
    const std::uint64_t last = le32(head, index + 4UL * kFrames) & ~1U;
    TEST_CHECK(first < last && last <= file.size());
    constexpr int kSeeds = 240;
    constexpr int kFlipsEach = 1;
    int failed = 0;
    int decoded = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
      std::mt19937 random(static_cast<unsigned>(seed) * 7919U);
      std::vector<std::pair<std::uint64_t, std::uint8_t>> flips;
      for (int i = 0; i < kFlipsEach; ++i) {
        flips.emplace_back(first + random() % (last - first), static_cast<std::uint8_t>(1U << (random() % 8U)));
      }
      const auto clean = file.reader();
      const luciole::media::bink2::Stream::Read damaged = [&](std::uint64_t offset, void* out, std::size_t count) {
        if (!clean(offset, out, count)) {
          return false;
        }
        for (const auto& [at, mask] : flips) {
          if (at >= offset && at < offset + count) {
            static_cast<std::uint8_t*>(out)[at - offset] ^= mask;
          }
        }
        return true;
      };
      auto fastStream = luciole::media::bink2::Stream::open(damaged, file.size());
      auto plainStream = luciole::media::bink2::Stream::open(damaged, file.size());
      TEST_CHECK(fastStream.has_value() && plainStream.has_value());
      const auto& layout = (*fastStream)->layout();
      Picture fastBefore(layout);
      Picture fastNow(layout);
      Picture plainBefore(layout);
      Picture plainNow(layout);
      for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
        bink2_idct_use_plain(0);
        const auto fast = (*fastStream)->decode(frame, fastNow.planes, frame == 0 ? nullptr : &fastBefore.planes);
        bink2_idct_use_plain(1);
        const auto plain = (*plainStream)->decode(frame, plainNow.planes, frame == 0 ? nullptr : &plainBefore.planes);
        TEST_CHECK(fast.has_value() == plain.has_value());
        if (!fast) {
          TEST_CHECK(fast.error() == plain.error());
          ++failed;
          break; // a later picture needs this one
        }
        ++decoded;
        for (std::size_t plane = 0; plane < 3; ++plane) {
          TEST_CHECK(std::memcmp(fastNow.planes.plane[plane], plainNow.planes.plane[plane], layout.size[plane]) == 0);
        }
        std::swap(fastBefore, fastNow);
        std::swap(plainBefore, plainNow);
      }
    }
    bink2_idct_use_plain(0);
    std::println("damaged pictures: {} decoded the same, {} streams ended the same way in an error", decoded, failed);
    TEST_CHECK(decoded > 0);
  }

} // namespace

int main() {
  extremeBlocks();
  randomBlocks();
  motionCompensation();
  samplePictures();
  damagedPictures();
  std::println("bink2-idct: all checks passed");
  return 0;
}
