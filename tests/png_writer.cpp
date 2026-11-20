#include "png_writer.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <random>
#include <string_view>
#include <system_error>
#include <vector>
#include <zlib.h>

namespace test {
  namespace {
    void appendBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value) {
      out.push_back(static_cast<std::uint8_t>(value >> 24));
      out.push_back(static_cast<std::uint8_t>(value >> 16));
      out.push_back(static_cast<std::uint8_t>(value >> 8));
      out.push_back(static_cast<std::uint8_t>(value));
    }

    // Length, type, data and the CRC of type and data.
    void appendChunk(std::vector<std::uint8_t>& out, std::string_view type, const std::vector<std::uint8_t>& data) {
      appendBigEndian(out, static_cast<std::uint32_t>(data.size()));
      const std::size_t start = out.size();
      out.insert(out.end(), type.begin(), type.end());
      out.insert(out.end(), data.begin(), data.end());
      const auto crc = crc32(crc32(0, nullptr, 0), out.data() + start, static_cast<uInt>(out.size() - start));
      appendBigEndian(out, static_cast<std::uint32_t>(crc));
    }

    // Nothing at `path`, or a regular file its owner may write.
    bool replaceable(const std::filesystem::path& path) {
      std::error_code ignored;
      const auto status = std::filesystem::status(path, ignored);
      return status.type() == std::filesystem::file_type::not_found
          || (std::filesystem::is_regular_file(status)
              && (status.permissions() & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
    }

    // A file beside `path` that this call created, open for writing; empty when none could be created.
    std::filesystem::path openNewSibling(const std::filesystem::path& path, std::ofstream& out) {
      constexpr int kAttempts = 8;
      std::random_device random;
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        auto candidate = path;
        candidate += std::format(".{:08x}.tmp", random());
        out.open(candidate, std::ios::binary | std::ios::noreplace);
        if (out.is_open()) {
          return candidate;
        }
        out.clear();
      }
      return {};
    }
  } // namespace

  bool writePng(const GlPixels& pixels, const std::filesystem::path& path) {
    // One IDAT chunk holds the whole image, so its compressed size must fit a chunk length and a uInt.
    constexpr std::size_t kMaxRawBytes = std::size_t{1} << 30;
    if (pixels.width <= 0 || pixels.height <= 0) {
      return false;
    }
    const auto row = static_cast<std::size_t>(pixels.width) * 4;
    const auto height = static_cast<std::size_t>(pixels.height);
    if (row > kMaxRawBytes / height || pixels.rgba.size() != row * height) {
      return false;
    }

    // Each row starts with filter type 0, so the rows are stored as they are.
    std::vector<std::uint8_t> raw;
    raw.reserve((row + 1) * height);
    for (std::size_t y = 0; y < height; ++y) {
      raw.push_back(0);
      const auto first = pixels.rgba.begin() + static_cast<std::ptrdiff_t>(y * row);
      raw.insert(raw.end(), first, first + static_cast<std::ptrdiff_t>(row));
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED)
        != Z_OK) {
      return false;
    }
    compressed.resize(compressedSize);

    std::vector<std::uint8_t> header;
    appendBigEndian(header, static_cast<std::uint32_t>(pixels.width));
    appendBigEndian(header, static_cast<std::uint32_t>(pixels.height));
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, adaptive filtering, no interlace

    std::vector<std::uint8_t> file{137, 80, 78, 71, 13, 10, 26, 10};
    appendChunk(file, "IHDR", header);
    appendChunk(file, "IDAT", compressed);
    appendChunk(file, "IEND", {});

    if (!replaceable(path)) {
      return false;
    }
    std::ofstream out;
    const auto partial = openNewSibling(path, out);
    if (partial.empty()) {
      return false;
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    out.close();
    std::error_code error;
    if (out.good()) {
      std::filesystem::rename(partial, path, error);
      if (!error) {
        return true;
      }
    }
    std::filesystem::remove(partial, error);
    return false;
  }
} // namespace test
