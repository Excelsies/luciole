#pragma once

#include "media/bink2_stream.h"
#include "test_check.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// What the Bink 2 tests share. Tests that need a real Bink 2 file take its path from LUCIOLE_BINK2_SAMPLE (a file
// with sound from LUCIOLE_BINK2_AUDIO_SAMPLE) and skip, with meson's skip exit code, when it is not set: no Bink 2
// file is kept in the repository.
namespace test::bink2 {
  namespace fs = std::filesystem;

  // Meson reads this exit code as a skipped test.
  inline constexpr int kSkipExitCode = 77;
  inline constexpr const char* kSampleVariable = "LUCIOLE_BINK2_SAMPLE";
  inline constexpr const char* kAudioSampleVariable = "LUCIOLE_BINK2_AUDIO_SAMPLE";

  // The file the variable names, or none when it is unset; a set variable that names no file fails the check.
  inline std::optional<fs::path> sampleFrom(const char* variable) {
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    fs::path path(value);
    std::error_code error;
    TEST_CHECK(path.is_absolute() && fs::is_regular_file(path, error));
    return path;
  }

  // Ends the test as skipped when `sample` is empty.
  inline const fs::path& needSample(const std::optional<fs::path>& sample, const char* variable) {
    if (!sample) {
      std::println("skipped: {} names no Bink 2 file", variable);
      std::exit(kSkipExitCode);
    }
    return *sample;
  }

  // Reads a file with pread, so no reader keeps a position.
  class FileReader {
  public:
    explicit FileReader(const fs::path& path) : m_fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {
      TEST_CHECK(m_fd >= 0);
      struct stat status{};
      TEST_CHECK(::fstat(m_fd, &status) == 0);
      m_size = static_cast<std::uint64_t>(status.st_size);
    }
    FileReader(const FileReader&) = delete;
    FileReader& operator=(const FileReader&) = delete;
    ~FileReader() { ::close(m_fd); }

    std::uint64_t size() const { return m_size; }
    luciole::media::bink2::Stream::Read reader() const {
      return [this](std::uint64_t offset, void* destination, std::size_t count) {
        auto* out = static_cast<std::uint8_t*>(destination);
        while (count > 0) {
          const auto got = ::pread(m_fd, out, count, static_cast<off_t>(offset));
          if (got <= 0) {
            return false;
          }
          out += got;
          offset += static_cast<std::uint64_t>(got);
          count -= static_cast<std::size_t>(got);
        }
        return true;
      };
    }

  private:
    int m_fd;
    std::uint64_t m_size = 0;
  };

  // A Bink 2 container made by hand: a header, the frame index, and the given frame packets.
  struct Container {
    char revision = 'n';
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    std::uint32_t rateNumerator = 30;
    std::uint32_t rateDenominator = 1;
    std::uint32_t flags = 0x2;
    std::uint32_t tracks = 0;
    std::uint32_t maxDecoded = 4096; // of each sound track
    std::vector<std::vector<std::uint8_t>> frames;
    // Overrides what the header says of the frame count and the file size, when set.
    std::optional<std::uint32_t> claimedFrames;

    std::vector<std::uint8_t> bytes() const {
      std::vector<std::uint8_t> out;
      const auto put = [&out](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
          out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
      };
      const auto count = static_cast<std::uint32_t>(frames.size());
      const std::uint32_t indexAt = 44 + 4 + 4 * tracks * 3;
      std::uint32_t at = indexAt + 4 * (count + 1);
      std::uint32_t total = at;
      for (const auto& frame : frames) {
        total += static_cast<std::uint32_t>(frame.size());
      }
      out.insert(out.end(), {'K', 'B', '2', static_cast<std::uint8_t>(revision)});
      put(total - 8);
      put(claimedFrames.value_or(count));
      put(0);
      put(0);
      put(width);
      put(height);
      put(rateNumerator);
      put(rateDenominator);
      put(flags);
      put(tracks);
      put(0x87e60500);
      for (std::uint32_t i = 0; i < tracks * 3; ++i) {
        put(i < tracks ? maxDecoded : (i < 2 * tracks ? 0x70U << 24 | 48000 : i));
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        put(at | 1U);
        at += static_cast<std::uint32_t>(frames[i].size());
      }
      put(at);
      for (const auto& frame : frames) {
        out.insert(out.end(), frame.begin(), frame.end());
      }
      return out;
    }
  };

  // The in-memory reader for `bytes`.
  inline luciole::media::bink2::Stream::Read memoryReader(const std::vector<std::uint8_t>& bytes) {
    return [&bytes](std::uint64_t offset, void* destination, std::size_t count) {
      if (offset > bytes.size() || count > bytes.size() - offset) {
        return false;
      }
      std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, static_cast<std::uint8_t*>(destination));
      return true;
    };
  }

  // The plane buffers one picture needs.
  struct Picture {
    explicit Picture(const luciole::media::bink2::Layout& layout)
        : memory(layout.total),
          planes{{memory.data(), memory.data() + layout.size[0], memory.data() + layout.size[0] + layout.size[1]}} {}
    std::vector<std::uint8_t> memory;
    luciole::media::bink2::Planes planes;
  };

  inline std::uint32_t le32(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at])
        | static_cast<std::uint32_t>(bytes[at + 1]) << 8U
        | static_cast<std::uint32_t>(bytes[at + 2]) << 16U
        | static_cast<std::uint32_t>(bytes[at + 3]) << 24U;
  }

  // A container of one frame made from the first key frame of the real file `sample` (its picture and header facts),
  // with one sound track whose chunk is `soundClaim` as the stated size and `chunkBytes` bytes long in all, zeros
  // after the claim. `maxDecoded` is what the track's table allows.
  inline Container
  withSoundChunk(const fs::path& sample, std::uint32_t soundClaim, std::uint32_t chunkBytes, std::uint32_t maxDecoded) {
    std::ifstream in(sample, std::ios::binary);
    std::vector<std::uint8_t> head(4096);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    TEST_CHECK(in.gcount() == static_cast<std::streamsize>(head.size()));
    const std::uint32_t tracks = le32(head, 40);
    const std::size_t index = 44 + 4 + 12UL * tracks;
    const std::size_t first = le32(head, index) & ~1U;
    const std::size_t end = le32(head, index + 4) & ~1U;
    TEST_CHECK(first < end && end - first < (64U << 20));
    std::vector<std::uint8_t> frame(end - first);
    in.seekg(static_cast<std::streamoff>(first));
    in.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    // The video follows the file's own sound chunks, if it has any.
    std::size_t at = 0;
    for (std::uint32_t track = 0; track < tracks; ++track) {
      at += 4 + le32(frame, at);
    }
    Container container;
    container.revision = static_cast<char>(head[3]);
    container.width = le32(head, 20);
    container.height = le32(head, 24);
    container.rateNumerator = le32(head, 28);
    container.rateDenominator = le32(head, 32);
    container.flags = le32(head, 36) & ~0x00100000U;
    container.tracks = 1;
    container.maxDecoded = maxDecoded;
    std::vector<std::uint8_t> chunk(4 + chunkBytes, 0);
    for (int i = 0; i < 4; ++i) {
      chunk[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(chunkBytes >> (8 * i));
      chunk[4 + static_cast<std::size_t>(i)] = chunkBytes >= 4 ? static_cast<std::uint8_t>(soundClaim >> (8 * i)) : 0;
    }
    chunk.insert(chunk.end(), frame.begin() + static_cast<std::ptrdiff_t>(at), frame.end());
    container.frames = {chunk};
    return container;
  }
} // namespace test::bink2
