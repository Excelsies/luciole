#include "media/bink2_poster.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fcntl.h>
#include <print>
#include <string>
#include <unistd.h>

namespace {
  constexpr int kInput = 3;
  constexpr int kMaxSide = 2048;
  constexpr std::size_t kMaxReason = 768;

  bool writeAll(int output, const std::uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
      const auto count = ::write(output, data + offset, size - offset);
      if (count <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }

  // The result file: a header, then a body; created new, never followed through a link.
  bool writeResult(std::string_view path, std::span<const std::uint8_t> header, std::span<const std::uint8_t> body) {
    const int output = ::open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0) {
      return false;
    }
    const bool written = writeAll(output, header.data(), header.size()) && writeAll(output, body.data(), body.size());
    return ::close(output) == 0 && written;
  }
} // namespace

int luciole::media::runBink2PosterWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 2) {
    return 2;
  }
  int side = 0;
  const auto parsed = std::from_chars(arguments[0].data(), arguments[0].data() + arguments[0].size(), side);
  if (parsed.ec != std::errc{}
      || parsed.ptr != arguments[0].data() + arguments[0].size()
      || side < 1
      || side > kMaxSide) {
    return 2;
  }
  try {
    const auto poster = bink2Poster(kInput, side, [] { return false; });
    if (!poster) {
      // One line, printable, bounded, and never the file's name.
      std::string reason = poster.error().substr(0, kMaxReason);
      std::ranges::replace_if(reason, [](unsigned char c) { return c < 32 || c > 126; }, '?');
      constexpr std::array<std::uint8_t, 4> header{'N', 'F', 'E', '1'};
      (void)writeResult(arguments[1], header, {reinterpret_cast<const std::uint8_t*>(reason.data()), reason.size()});
      std::println(stderr, "{}", reason);
      return 1;
    }
    std::array<std::uint8_t, 12> header{'N', 'F', 'R', '1'};
    for (int i = 0; i < 4; ++i) {
      header[4 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(poster->width) >> (24 - i * 8));
      header[8 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(poster->height) >> (24 - i * 8));
    }
    if (!writeResult(arguments[1], header, poster->rgba)) {
      std::println(stderr, "the result could not be written");
      return 1;
    }
    return 0;
  } catch (const std::exception&) {
    std::println(stderr, "out of memory or internal error");
    return 1;
  }
}
