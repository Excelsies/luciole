#include "png_writer.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <system_error>
#include <vector>
#include <zlib.h>

namespace {
  using test::GlPixels;

  // Every byte of the image differs from its neighbors, so a swapped or shifted row shows.
  GlPixels pattern(int width, int height, std::uint8_t seed) {
    GlPixels pixels{.width = width, .height = height, .rgba = {}};
    pixels.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < pixels.rgba.size(); ++i) {
      pixels.rgba[i] = static_cast<std::uint8_t>(seed + i * 37);
    }
    return pixels;
  }

  std::vector<std::uint8_t> bytesOf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    TEST_CHECK(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  std::uint32_t bigEndian(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (std::uint32_t{bytes[offset]} << 24)
        | (std::uint32_t{bytes[offset + 1]} << 16)
        | (std::uint32_t{bytes[offset + 2]} << 8)
        | std::uint32_t{bytes[offset + 3]};
  }

  // Reads the file back chunk by chunk: valid checksums, the header of an 8-bit RGBA image of the pixels' size,
  // and rows that inflate to the pixels.
  void pngHoldsPixels(const std::filesystem::path& path, const GlPixels& pixels) {
    const auto file = bytesOf(path);
    constexpr std::array<std::uint8_t, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
    TEST_CHECK(file.size() > kSignature.size());
    TEST_CHECK(std::equal(kSignature.begin(), kSignature.end(), file.begin()));
    std::vector<std::uint8_t> compressed;
    std::vector<std::string> order;
    std::size_t offset = kSignature.size();
    for (int chunk = 0; chunk < 64 && offset + 12 <= file.size(); ++chunk) {
      const std::uint32_t length = bigEndian(file, offset);
      TEST_CHECK(offset + 12 + length <= file.size());
      const std::string type(
          file.begin() + static_cast<std::ptrdiff_t>(offset + 4), file.begin() + static_cast<std::ptrdiff_t>(offset + 8)
      );
      const auto* body = file.data() + offset + 8;
      const auto crc = crc32(crc32(0, nullptr, 0), file.data() + offset + 4, length + 4);
      TEST_CHECK(crc == bigEndian(file, offset + 8 + length));
      if (type == "IHDR") {
        TEST_CHECK(length == 13);
        const std::vector<std::uint8_t> header(body, body + 13);
        TEST_CHECK(bigEndian(header, 0) == static_cast<std::uint32_t>(pixels.width));
        TEST_CHECK(bigEndian(header, 4) == static_cast<std::uint32_t>(pixels.height));
        TEST_CHECK(header[8] == 8);
        TEST_CHECK(header[9] == 6);
      } else if (type == "IDAT") {
        compressed.insert(compressed.end(), body, body + length);
      }
      order.push_back(type);
      offset += 12 + length;
    }
    TEST_CHECK(offset == file.size());
    TEST_CHECK(order.size() >= 3);
    TEST_CHECK(order.front() == "IHDR");
    TEST_CHECK(order.back() == "IEND");
    const auto row = static_cast<std::size_t>(pixels.width) * 4;
    std::vector<std::uint8_t> rows((row + 1) * static_cast<std::size_t>(pixels.height));
    uLongf size = rows.size();
    TEST_CHECK(uncompress(rows.data(), &size, compressed.data(), compressed.size()) == Z_OK);
    TEST_CHECK(size == rows.size());
    std::size_t mismatches = 0;
    for (std::size_t y = 0; y < static_cast<std::size_t>(pixels.height); ++y) {
      mismatches += rows[y * (row + 1)] != 0 ? 1 : 0;
      for (std::size_t i = 0; i < row; ++i) {
        mismatches += rows[y * (row + 1) + 1 + i] != pixels.rgba[y * row + i] ? 1 : 0;
      }
    }
    TEST_CHECK(mismatches == 0);
  }

  std::ptrdiff_t entriesIn(const std::filesystem::path& dir) {
    return std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{});
  }

  void writesAndReplaces(const std::filesystem::path& dir) {
    const auto path = dir / "scene.png";
    const auto first = pattern(7, 5, 0);
    TEST_CHECK(test::writePng(first, path));
    pngHoldsPixels(path, first);
    const auto second = pattern(3, 9, 11);
    TEST_CHECK(test::writePng(second, path));
    pngHoldsPixels(path, second);
  }

  // A failed write leaves nothing new behind and never removes or changes what was at the path.
  void failureLeavesPathAlone(const std::filesystem::path& dir) {
    const auto pixels = pattern(4, 4, 3);
    TEST_CHECK(!test::writePng(GlPixels{}, dir / "empty.png"));
    TEST_CHECK(!std::filesystem::exists(dir / "empty.png"));
    TEST_CHECK(!test::writePng(GlPixels{.width = 2, .height = 2, .rgba = {1, 2, 3}}, dir / "short.png"));
    TEST_CHECK(!std::filesystem::exists(dir / "short.png"));
    TEST_CHECK(!test::writePng(pixels, dir / "missing" / "scene.png"));
    TEST_CHECK(!std::filesystem::exists(dir / "missing"));

    const auto folder = dir / "folder.png";
    TEST_CHECK(std::filesystem::create_directory(folder));
    TEST_CHECK(!test::writePng(pixels, folder));
    TEST_CHECK(std::filesystem::is_directory(folder));

    const auto locked = dir / "locked.png";
    {
      std::ofstream out(locked, std::ios::binary);
      out << "kept";
    }
    std::filesystem::permissions(locked, std::filesystem::perms::owner_read);
    TEST_CHECK(!test::writePng(pixels, locked));
    TEST_CHECK((bytesOf(locked) == std::vector<std::uint8_t>{'k', 'e', 'p', 't'}));
  }
} // namespace

int main() {
  const luciole::test::TempFolder temporary("luciole-png-writer");
  const std::filesystem::path& dir = temporary.path();
  writesAndReplaces(dir);
  failureLeavesPathAlone(dir);
  // scene.png, folder.png and locked.png, and no partial file beside them.
  TEST_CHECK(entriesIn(dir) == 3);
  std::println("png writer checks passed");
}
