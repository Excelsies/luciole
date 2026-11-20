#include "media/bink2_poster.h"

#include "media/bink2_stream.h"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  using luciole::media::Bink2Poster;
  using luciole::media::bink2::Layout;
  using luciole::media::bink2::Planes;
  using luciole::media::bink2::Stream;

  // A picture whose sampled luma is below this almost everywhere is black.
  constexpr int kBlackLuma = 12;
  constexpr int kSampleStep = 4;

  struct Picture {
    explicit Picture(const Layout& layout) : memory(layout.total) {
      planes.plane = {memory.data(), memory.data() + layout.size[0], memory.data() + layout.size[0] + layout.size[1]};
    }
    std::vector<std::uint8_t> memory;
    Planes planes;
  };

  struct Brightness {
    double mean = 0;
    bool black = false;
  };

  Brightness measure(const Picture& picture, const Layout& layout, int width, int height) {
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    std::uint64_t dark = 0;
    for (int y = 0; y < height; y += kSampleStep) {
      const std::uint8_t* row = picture.planes.plane[0] + static_cast<std::size_t>(y) * layout.stride[0];
      for (int x = 0; x < width; x += kSampleStep) {
        sum += row[x];
        dark += row[x] < kBlackLuma ? 1 : 0;
        ++count;
      }
    }
    return {
        .mean = count != 0 ? static_cast<double>(sum) / static_cast<double>(count) : 0,
        .black = count != 0 && dark * 100 >= count * 98
    };
  }

  std::uint8_t clip(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }

  // Area average of the planes into at most side by side, converted with the full-range BT.601 matrix the player's
  // caps name.
  Bink2Poster fitted(const Picture& picture, const Layout& layout, int width, int height, int side) {
    const double scale = std::min({1.0, static_cast<double>(side) / width, static_cast<double>(side) / height});
    const int outWidth = std::max(1, static_cast<int>(width * scale));
    const int outHeight = std::max(1, static_cast<int>(height * scale));
    Bink2Poster poster{.width = outWidth, .height = outHeight, .rgba = {}};
    poster.rgba.resize(static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4);
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;
    for (int oy = 0; oy < outHeight; ++oy) {
      const int y0 = oy * height / outHeight;
      const int y1 = std::max(y0 + 1, (oy + 1) * height / outHeight);
      for (int ox = 0; ox < outWidth; ++ox) {
        const int x0 = ox * width / outWidth;
        const int x1 = std::max(x0 + 1, (ox + 1) * width / outWidth);
        std::uint64_t luma = 0;
        for (int y = y0; y < y1; ++y) {
          const std::uint8_t* row = picture.planes.plane[0] + static_cast<std::size_t>(y) * layout.stride[0];
          for (int x = x0; x < x1; ++x) {
            luma += row[x];
          }
        }
        const int cx0 = x0 / 2;
        const int cx1 = std::min(chromaWidth, std::max(cx0 + 1, (x1 + 1) / 2));
        const int cy0 = y0 / 2;
        const int cy1 = std::min(chromaHeight, std::max(cy0 + 1, (y1 + 1) / 2));
        std::uint64_t blue = 0;
        std::uint64_t red = 0;
        for (int y = cy0; y < cy1; ++y) {
          const std::uint8_t* u = picture.planes.plane[1] + static_cast<std::size_t>(y) * layout.stride[1];
          const std::uint8_t* v = picture.planes.plane[2] + static_cast<std::size_t>(y) * layout.stride[2];
          for (int x = cx0; x < cx1; ++x) {
            blue += u[x];
            red += v[x];
          }
        }
        const auto lumaCount = static_cast<std::uint64_t>((y1 - y0) * (x1 - x0));
        const auto chromaCount = static_cast<std::uint64_t>((cy1 - cy0) * (cx1 - cx0));
        const int y = static_cast<int>(luma / lumaCount);
        const int u = static_cast<int>(blue / chromaCount) - 128;
        const int v = static_cast<int>(red / chromaCount) - 128;
        std::uint8_t* out = &poster.rgba[(static_cast<std::size_t>(oy) * outWidth + ox) * 4];
        out[0] = clip(y + ((359 * v + 128) >> 8));
        out[1] = clip(y - ((88 * u + 183 * v + 128) >> 8));
        out[2] = clip(y + ((454 * u + 128) >> 8));
        out[3] = 255;
      }
    }
    return poster;
  }
} // namespace

std::expected<luciole::media::Bink2Poster, std::string>
luciole::media::bink2Poster(int fd, int side, const std::function<bool()>& cancelled) {
  struct stat status{};
  if (side < 1 || ::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) {
    return std::unexpected("not a regular file");
  }
  auto opened = Stream::open(
      [fd](std::uint64_t offset, void* destination, std::size_t count) {
        auto* out = static_cast<std::uint8_t*>(destination);
        // Each pass reads at least one byte or ends, so at most `count` passes.
        while (count > 0) {
          const ssize_t got = ::pread(fd, out, count, static_cast<off_t>(offset));
          if (got <= 0) {
            return false;
          }
          out += got;
          offset += static_cast<std::uint64_t>(got);
          count -= static_cast<std::size_t>(got);
        }
        return true;
      },
      static_cast<std::uint64_t>(status.st_size)
  );
  if (!opened) {
    return std::unexpected(std::move(opened.error()));
  }
  Stream& stream = **opened;
  const auto& layout = stream.layout();
  const int width = stream.info().width;
  const int height = stream.info().height;
  Picture before(layout);
  Picture now(layout);
  Picture best(layout);
  double bestMean = -1;
  const std::uint64_t frames = std::min<std::uint64_t>(stream.info().frames, kPosterFrames);
  for (std::uint64_t frame = 0; frame < frames; ++frame) {
    if (cancelled()) {
      return std::unexpected("thumbnail request cancelled");
    }
    if (const auto decoded = stream.decode(frame, now.planes, frame == 0 ? nullptr : &before.planes); !decoded) {
      return std::unexpected(decoded.error());
    }
    const auto light = measure(now, layout, width, height);
    if (!light.black) {
      return fitted(now, layout, width, height, side);
    }
    if (light.mean > bestMean) {
      bestMean = light.mean;
      best.memory = now.memory;
    }
    std::swap(before, now);
  }
  // Every frame looked at is black: a dark video still gets its brightest.
  return fitted(best, layout, width, height, side);
}
