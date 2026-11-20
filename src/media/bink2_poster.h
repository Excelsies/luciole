#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::media {
  // One picture of a Bink 2 file, RGBA with rows packed at width * 4 bytes.
  struct Bink2Poster {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
  };

  // The most frames decoded from the start of a file to find its poster.
  inline constexpr int kPosterFrames = 30;

  // A picture of the file, fitted into `side` by `side` pixels and never enlarged: the first of the first
  // kPosterFrames frames that is not black, or the brightest of them when all are. It decodes on the calling thread,
  // with the player's header checks and size limits, reads only with pread, and asks `cancelled` between frames. Any
  // failure, damage in those frames included, is an error that says why.
  std::expected<Bink2Poster, std::string> bink2Poster(int fd, int side, const std::function<bool()>& cancelled);

  // The poster helper, run as a mode of the application binary like the other thumbnail workers, never in the
  // process that browses: arguments are the side and the output path; the file is descriptor 3. It writes the
  // thumbnail workers' private result (NFR1 with the size and RGBA rows, or NFE1 with a one-line reason) to the new
  // output file, prints the reason on stderr and returns 1 on any failure, 2 for bad arguments. It starts no
  // GStreamer and writes nothing else. Without playback in the build it only fails.
  int runBink2PosterWorker(std::span<const std::string_view> arguments);

  // The facts of a Bink 2 file for the details rows, read from its header only, with the playback header checks and
  // no decoding: the file is descriptor 3, and standard output gets the JSON that ffprobe gives for other videos, in
  // the fields the rows read (format, duration, one video stream, a stream per sound track). A file that fails the
  // checks gets the reason on stderr, nothing on standard output, and status 1. Without playback it only fails.
  int runBink2FactsWorker(std::span<const std::string_view> arguments);
} // namespace luciole::media
