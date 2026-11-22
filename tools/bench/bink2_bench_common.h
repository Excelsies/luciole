// Shared by the Bink 2 bench modes.
#pragma once

#include "media/bink2_stream.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <print>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace bink2bench {
  using Clock = std::chrono::steady_clock;
  using luciole::media::bink2::Planes;
  using luciole::media::bink2::Stream;

  // A whole number in [minimum, maximum] from the command line; anything else ends the program with a message.
  inline std::uint64_t number(const char* text, const char* what, std::uint64_t minimum, std::uint64_t maximum) {
    const std::string_view view(text);
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(view.data(), view.data() + view.size(), value);
    if (view.empty()
        || parsed.ec != std::errc{}
        || parsed.ptr != view.data() + view.size()
        || value < minimum
        || value > maximum) {
      std::println(stderr, "{} must be a whole number from {} to {}, not '{}'", what, minimum, maximum, view);
      std::exit(2);
    }
    return value;
  }

  class File {
  public:
    explicit File(const char* path) : m_fd(::open(path, O_RDONLY | O_CLOEXEC)) {
      struct stat status{};
      if (m_fd < 0 || ::fstat(m_fd, &status) != 0) {
        std::println(stderr, "cannot open {}", path);
        std::exit(2);
      }
      m_size = static_cast<std::uint64_t>(status.st_size);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File() { ::close(m_fd); }
    std::uint64_t size() const { return m_size; }
    Stream::Read reader() const {
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

  struct Pictures {
    explicit Pictures(const luciole::media::bink2::Layout& layout) : memory(layout.total) {
      planes.plane = {memory.data(), memory.data() + layout.size[0], memory.data() + layout.size[0] + layout.size[1]};
    }
    std::vector<std::uint8_t> memory;
    Planes planes;
  };

  inline std::unique_ptr<Stream> open(const File& file) {
    auto stream = Stream::open(file.reader(), file.size());
    if (!stream) {
      std::println(stderr, "{}", stream.error());
      std::exit(2);
    }
    return std::move(*stream);
  }

  // CPU seconds of the whole process, all threads.
  inline double cpuSeconds() {
    timespec now{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) / 1e9;
  }

  inline double milliseconds(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
  }
  int play(const char* path, int width, int height);
} // namespace bink2bench
