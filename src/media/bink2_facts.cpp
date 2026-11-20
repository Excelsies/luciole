#include "media/bink2_poster.h"
#include "media/bink2_stream.h"

#include <array>
#include <cstdio>
#include <format>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  constexpr int kInput = 3;
  constexpr std::size_t kHead = 44 + 4 + 7 * 12;

  std::uint32_t le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
  }
} // namespace

int luciole::media::runBink2FactsWorker(std::span<const std::string_view> arguments) {
  if (!arguments.empty()) {
    return 2;
  }
  struct stat status{};
  if (::fstat(kInput, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) {
    std::println(stderr, "not a regular file");
    return 1;
  }
  std::array<std::uint8_t, kHead> head{};
  const auto size = static_cast<std::uint64_t>(status.st_size);
  const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, head.size()));
  if (::pread(kInput, head.data(), count, 0) != static_cast<ssize_t>(count)) {
    std::println(stderr, "the file cannot be read");
    return 1;
  }
  if (const auto problem = bink2::checkContainerHead(std::span(head).first(count), size); !problem) {
    std::println(stderr, "{}", problem.error());
    return 1;
  }
  const std::uint32_t frames = le32(&head[8]);
  const std::uint32_t width = le32(&head[20]);
  const std::uint32_t height = le32(&head[24]);
  const std::uint32_t numerator = le32(&head[28]);
  const std::uint32_t denominator = le32(&head[32]);
  const std::uint32_t tracks = le32(&head[40]);
  const double seconds = static_cast<double>(frames) * denominator / numerator;
  std::string json = std::format(
      "{{\"format\":{{\"format_long_name\":\"Bink 2\",\"duration\":\"{:.6f}\"}},\"streams\":["
      "{{\"codec_type\":\"video\",\"codec_name\":\"Bink 2\",\"width\":{},\"height\":{},\"avg_frame_rate\":\"{}/{}\","
      "\"nb_frames\":\"{}\"}}",
      seconds, width, height, numerator, denominator, frames
  );
  const std::size_t tables = 44 + (head[3] >= 'i' ? 4U : 0U);
  // The track tables are in the 132 bytes read when the file is long enough; checkContainerHead refused one that is
  // not, so each track's rate and flags are there.
  for (std::uint32_t track = 0; track < tracks && tables + 8ULL * tracks <= count; ++track) {
    const std::uint8_t* description = &head[tables + 4ULL * tracks + 4ULL * track];
    const std::uint32_t rate = le32(description) & 0xFFFFFFU;
    const int channels = (description[3] & 0x20U) != 0 ? 2 : 1;
    const char* codec = (description[3] & 0x10U) != 0 ? "Bink Audio 2 (DCT)" : "Bink Audio 2 (RDFT)";
    json += std::format(
        ",{{\"codec_type\":\"audio\",\"codec_name\":\"{}\",\"sample_rate\":\"{}\",\"channels\":{},"
        "\"bits_per_sample\":16}}",
        codec, rate, channels
    );
  }
  json += "]}";
  return std::fwrite(json.data(), 1, json.size(), stdout) == json.size() && std::fflush(stdout) == 0 ? 0 : 1;
}
