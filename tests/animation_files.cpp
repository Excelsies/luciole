#include "animation_files.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <zlib.h>

namespace test {
  namespace {
    using Bytes = std::vector<std::uint8_t>;

    void appendText(Bytes& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

    void appendLittle(Bytes& out, std::uint32_t value, int bytes) {
      for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    }

    void appendBig(Bytes& out, std::uint32_t value, int bytes) {
      for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    }

    // Packs codes least significant bit first, as GIF and WebP lossless streams read them.
    class BitWriter {
    public:
      void put(std::uint32_t value, int bits) {
        for (int i = 0; i < bits; ++i) {
          if (m_used == 0) {
            m_bytes.push_back(0);
          }
          m_bytes.back() = static_cast<std::uint8_t>(m_bytes.back() | (((value >> i) & 1U) << m_used));
          m_used = (m_used + 1) % 8;
        }
      }
      const Bytes& bytes() const noexcept { return m_bytes; }

    private:
      Bytes m_bytes;
      int m_used = 0;
    };

    // LZW data of one palette index repeated: 9-bit literals with a clear code every 100, so the table never
    // needs wider codes.
    Bytes gifPixels(std::uint8_t index, std::size_t count) {
      constexpr std::uint32_t kClear = 256;
      constexpr std::uint32_t kEnd = 257;
      constexpr std::size_t kRun = 100;
      BitWriter bits;
      for (std::size_t i = 0; i < count; ++i) {
        if (i % kRun == 0) {
          bits.put(kClear, 9);
        }
        bits.put(index, 9);
      }
      bits.put(kEnd, 9);
      Bytes out{8};
      const auto& data = bits.bytes();
      for (std::size_t start = 0; start < data.size(); start += 255) {
        const auto size = std::min<std::size_t>(255, data.size() - start);
        out.push_back(static_cast<std::uint8_t>(size));
        out.insert(
            out.end(), data.begin() + static_cast<std::ptrdiff_t>(start),
            data.begin() + static_cast<std::ptrdiff_t>(start + size)
        );
      }
      out.push_back(0);
      return out;
    }

    void appendPngChunk(Bytes& out, std::string_view type, const Bytes& data) {
      appendBig(out, static_cast<std::uint32_t>(data.size()), 4);
      const auto start = out.size();
      appendText(out, type);
      out.insert(out.end(), data.begin(), data.end());
      const auto crc = crc32(crc32(0, nullptr, 0), out.data() + start, static_cast<uInt>(out.size() - start));
      appendBig(out, static_cast<std::uint32_t>(crc), 4);
    }

    Bytes deflatedRows(int width, int height, const FlatFrame& frame) {
      Bytes raw;
      for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const auto& rgba = frame.lower && y >= height / 2 ? *frame.lower : frame.rgba;
        for (int x = 0; x < width; ++x) {
          raw.insert(raw.end(), rgba.begin(), rgba.end());
        }
      }
      uLongf size = compressBound(static_cast<uLong>(raw.size()));
      Bytes out(size);
      if (compress2(out.data(), &size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
        throw std::runtime_error("deflate failed");
      }
      out.resize(size);
      return out;
    }

    void appendRiffChunk(Bytes& out, std::string_view type, const Bytes& data) {
      appendText(out, type);
      appendLittle(out, static_cast<std::uint32_t>(data.size()), 4);
      out.insert(out.end(), data.begin(), data.end());
      if (data.size() % 2 != 0) {
        out.push_back(0);
      }
    }

    // A lossless bitstream whose five prefix codes each hold one symbol, so every pixel is the same color and
    // takes no bits.
    Bytes flatLossless(int width, int height, const std::array<std::uint8_t, 4>& rgba) {
      BitWriter bits;
      bits.put(0x2F, 8);
      bits.put(static_cast<std::uint32_t>(width - 1), 14);
      bits.put(static_cast<std::uint32_t>(height - 1), 14);
      bits.put(1, 1); // alpha is used
      bits.put(0, 3); // version
      bits.put(0, 1); // no transform
      bits.put(0, 1); // no color cache
      bits.put(0, 1); // no meta prefix codes
      for (const std::uint8_t symbol : {rgba[1], rgba[0], rgba[2], rgba[3], std::uint8_t{0}}) {
        bits.put(1, 1); // simple code
        bits.put(0, 1); // one symbol
        if (symbol < 2) {
          bits.put(0, 1);
          bits.put(symbol, 1);
        } else {
          bits.put(1, 1);
          bits.put(symbol, 8);
        }
      }
      return bits.bytes();
    }
  } // namespace

  std::vector<std::uint8_t>
  animatedGif(int width, int height, const std::vector<FlatFrame>& frames, std::optional<std::uint16_t> loopExtension) {
    if (std::ranges::any_of(frames, [](const FlatFrame& frame) { return frame.lower.has_value(); })) {
      throw std::invalid_argument("GIF frames are a single color");
    }
    Bytes out;
    appendText(out, "GIF89a");
    appendLittle(out, static_cast<std::uint32_t>(width), 2);
    appendLittle(out, static_cast<std::uint32_t>(height), 2);
    out.insert(out.end(), {0xF7, 0, 0}); // a global table of 256 colors
    for (std::size_t i = 0; i < 256; ++i) {
      const auto& rgba = i < frames.size() ? frames[i].rgba : std::array<std::uint8_t, 4>{};
      out.insert(out.end(), {rgba[0], rgba[1], rgba[2]});
    }
    if (loopExtension) {
      out.insert(out.end(), {0x21, 0xFF, 0x0B});
      appendText(out, "NETSCAPE2.0");
      out.insert(out.end(), {0x03, 0x01});
      appendLittle(out, *loopExtension, 2);
      out.push_back(0);
    }
    for (std::size_t i = 0; i < frames.size() && i < 256; ++i) {
      out.insert(out.end(), {0x21, 0xF9, 0x04, 0x00});
      appendLittle(out, static_cast<std::uint32_t>(frames[i].delayMs / 10), 2);
      out.insert(out.end(), {0x00, 0x00});
      out.push_back(0x2C);
      appendLittle(out, 0, 4);
      appendLittle(out, static_cast<std::uint32_t>(width), 2);
      appendLittle(out, static_cast<std::uint32_t>(height), 2);
      out.push_back(0);
      const auto pixels = gifPixels(static_cast<std::uint8_t>(i), static_cast<std::size_t>(width) * height);
      out.insert(out.end(), pixels.begin(), pixels.end());
    }
    out.push_back(0x3B);
    return out;
  }

  std::vector<std::uint8_t>
  animatedPng(int width, int height, const std::vector<FlatFrame>& frames, std::uint32_t plays) {
    Bytes out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes header;
    appendBig(header, static_cast<std::uint32_t>(width), 4);
    appendBig(header, static_cast<std::uint32_t>(height), 4);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    appendPngChunk(out, "IHDR", header);
    Bytes control;
    appendBig(control, static_cast<std::uint32_t>(frames.size()), 4);
    appendBig(control, plays, 4);
    appendPngChunk(out, "acTL", control);
    std::uint32_t sequence = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      Bytes frame;
      appendBig(frame, sequence++, 4);
      appendBig(frame, static_cast<std::uint32_t>(width), 4);
      appendBig(frame, static_cast<std::uint32_t>(height), 4);
      appendBig(frame, 0, 8);
      appendBig(frame, static_cast<std::uint32_t>(frames[i].delayMs), 2);
      appendBig(frame, 1000, 2);
      frame.insert(frame.end(), {0, 0});
      appendPngChunk(out, "fcTL", frame);
      const auto data = deflatedRows(width, height, frames[i]);
      if (i == 0) {
        appendPngChunk(out, "IDAT", data);
      } else {
        Bytes next;
        appendBig(next, sequence++, 4);
        next.insert(next.end(), data.begin(), data.end());
        appendPngChunk(out, "fdAT", next);
      }
    }
    appendPngChunk(out, "IEND", {});
    return out;
  }

  std::vector<std::uint8_t>
  animatedWebp(int width, int height, const std::vector<FlatFrame>& frames, std::uint16_t loopCount) {
    if (std::ranges::any_of(frames, [](const FlatFrame& frame) { return frame.lower.has_value(); })) {
      throw std::invalid_argument("WebP frames are a single color");
    }
    Bytes body;
    appendText(body, "WEBP");
    Bytes extended{0x12, 0, 0, 0}; // alpha and animation
    appendLittle(extended, static_cast<std::uint32_t>(width - 1), 3);
    appendLittle(extended, static_cast<std::uint32_t>(height - 1), 3);
    appendRiffChunk(body, "VP8X", extended);
    Bytes animation{0, 0, 0, 0};
    appendLittle(animation, loopCount, 2);
    appendRiffChunk(body, "ANIM", animation);
    for (const auto& frame : frames) {
      Bytes payload;
      appendLittle(payload, 0, 6);
      appendLittle(payload, static_cast<std::uint32_t>(width - 1), 3);
      appendLittle(payload, static_cast<std::uint32_t>(height - 1), 3);
      appendLittle(payload, static_cast<std::uint32_t>(frame.delayMs), 3);
      payload.push_back(0x02); // replace the canvas, no blending
      appendRiffChunk(payload, "VP8L", flatLossless(width, height, frame.rgba));
      appendRiffChunk(body, "ANMF", payload);
    }
    Bytes out;
    appendText(out, "RIFF");
    appendLittle(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  }

  std::vector<std::uint8_t> stillPng(int width, int height, const std::array<std::uint8_t, 4>& rgba) {
    Bytes out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes header;
    appendBig(header, static_cast<std::uint32_t>(width), 4);
    appendBig(header, static_cast<std::uint32_t>(height), 4);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    appendPngChunk(out, "IHDR", header);
    appendPngChunk(out, "IDAT", deflatedRows(width, height, {.rgba = rgba, .delayMs = 0, .lower = std::nullopt}));
    appendPngChunk(out, "IEND", {});
    return out;
  }

  std::vector<std::uint8_t> stillWebp(int width, int height, const std::array<std::uint8_t, 4>& rgba, bool extended) {
    Bytes body;
    appendText(body, "WEBP");
    if (extended) {
      Bytes header{0x10, 0, 0, 0}; // alpha only
      appendLittle(header, static_cast<std::uint32_t>(width - 1), 3);
      appendLittle(header, static_cast<std::uint32_t>(height - 1), 3);
      appendRiffChunk(body, "VP8X", header);
    }
    appendRiffChunk(body, "VP8L", flatLossless(width, height, rgba));
    Bytes out;
    appendText(out, "RIFF");
    appendLittle(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  }

  bool writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::noreplace);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return out.good();
  }
} // namespace test
