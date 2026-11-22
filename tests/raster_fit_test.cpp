#include "imaging/raster_decoder.h"
#include "render/core/image_resize.h"
#include "test_check.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <print>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
  struct PixbufDeleter {
    void operator()(GdkPixbuf* pixbuf) const { g_object_unref(pixbuf); }
  };
  using Pixbuf = std::unique_ptr<GdkPixbuf, PixbufDeleter>;

  // Colors that differ from pixel to pixel. Shrunk to 64 pixels across, the 2001 by 1500 picture of them comes out one
  // level apart in one byte between the alpha-weighted path and the opaque path, which is what the opaque test needs.
  std::uint8_t channel(int x, int y, int c) {
    const std::uint32_t noise = (static_cast<std::uint32_t>(x) * 374761393U
                                 + static_cast<std::uint32_t>(y) * 668265263U
                                 + static_cast<std::uint32_t>(c) * 2147483647U
                                 + 32U * 2654435761U)
        >> 13;
    return static_cast<std::uint8_t>((x * 7 + y * 13 + c * 61 + static_cast<int>(noise & 15U)) & 255);
  }

  Pixbuf make(int width, int height, bool hasAlpha, int alpha) {
    Pixbuf pixbuf(gdk_pixbuf_new(GDK_COLORSPACE_RGB, hasAlpha, 8, width, height));
    const int channels = hasAlpha ? 4 : 3, stride = gdk_pixbuf_get_rowstride(pixbuf.get());
    auto* pixels = gdk_pixbuf_get_pixels(pixbuf.get());
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        auto* pixel = pixels + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * channels;
        for (int c = 0; c < 3; ++c) {
          pixel[c] = channel(x, y, c);
        }
        if (hasAlpha) {
          pixel[3] = alpha >= 0 ? static_cast<std::uint8_t>(alpha) : channel(x, y, 3);
        }
      }
    }
    return pixbuf;
  }

  // The same pixels in rows with no padding.
  std::vector<std::uint8_t> packed(GdkPixbuf* pixbuf) {
    const int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
    const int row = width * gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
    const auto* pixels = gdk_pixbuf_read_pixels(pixbuf);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(row) * height);
    for (int y = 0; y < height; ++y) {
      std::memcpy(out.data() + static_cast<std::size_t>(y) * row, pixels + static_cast<std::size_t>(y) * stride, row);
    }
    return out;
  }

  using Color = std::function<std::uint8_t(int x, int y, int c)>;
  using Alpha = std::function<std::uint8_t(int x, int y)>;

  // A picture whose rows are `extra` bytes longer than its pixels. With `exact` the buffer ends with the last pixel, so
  // the last row is shorter than the stride and a read beyond the pixels is a read past the buffer.
  struct Rows {
    std::unique_ptr<std::uint8_t[]> bytes;
    Pixbuf pixbuf;
  };
  Rows makeRows(int width, int height, int channels, int extra, bool exact, const Color& color, const Alpha& alpha) {
    Rows rows;
    const int stride = width * channels + extra;
    const std::size_t size = exact
        ? static_cast<std::size_t>(height - 1) * stride + static_cast<std::size_t>(width) * channels
        : static_cast<std::size_t>(stride) * height;
    rows.bytes = std::make_unique<std::uint8_t[]>(size);
    std::memset(rows.bytes.get(), 0x5a, size);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        auto* pixel = rows.bytes.get() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * channels;
        for (int c = 0; c < 3; ++c) {
          pixel[c] = color(x, y, c);
        }
        if (channels == 4) {
          pixel[3] = alpha(x, y);
        }
      }
    }
    rows.pixbuf.reset(gdk_pixbuf_new_from_data(
        rows.bytes.get(), GDK_COLORSPACE_RGB, channels == 4, 8, width, height, stride, nullptr, nullptr
    ));
    return rows;
  }
  const Color kColors = [](int x, int y, int c) { return channel(x, y, c); };
  const Alpha kOpaque = [](int, int) -> std::uint8_t { return 255; };
  const Alpha kVaried = [](int x, int y) { return channel(x, y, 3); };

  ResizeSourceLayout layoutOf(int channels) {
    return channels == 4 ? ResizeSourceLayout::Rgba : ResizeSourceLayout::Rgb;
  }

  // Rows padded to four bytes are what gdk_pixbuf_new gives for a width that is not a multiple of four; four-channel
  // rows and rows that end at the last pixel are made by hand.
  void rowsWithPaddingGiveWhatPackedRowsGive() {
    const auto check = [](GdkPixbuf* pixbuf, int channels) {
      const auto fitted = luciole::fitRasterPixels(pixbuf, 128, 10'000'000);
      TEST_CHECK(fitted && fitted->width == 128 && fitted->height == 82);
      const auto rows = packed(pixbuf);
      TEST_CHECK(fitted->rgba == resizeToRgba(rows.data(), 1001, 640, layoutOf(channels), 128, 82));
    };
    const auto rgb = make(1001, 640, false, -1);
    TEST_CHECK(gdk_pixbuf_get_rowstride(rgb.get()) != 1001 * 3);
    check(rgb.get(), 3);
    check(make(1001, 640, true, -1).get(), 4);
    for (const int channels : {3, 4}) {
      for (const bool exact : {false, true}) {
        const auto rows = makeRows(1001, 640, channels, 12, exact, kColors, kVaried);
        check(rows.pixbuf.get(), channels);
      }
    }
  }

  // Pictures whose alpha is 255 everywhere are shrunk as opaque: exactly the opaque resizer's colors, alpha exactly
  // 255, and within one level of the colors the same picture without alpha gives.
  void anOpaqueAlphaChannelIsShrunkAsOpaque() {
    const auto plain = luciole::fitRasterPixels(make(2001, 1500, false, -1).get(), 64, 10'000'000);
    TEST_CHECK(plain && plain->width == 64 && plain->height == 48);
    const auto check = [&](GdkPixbuf* pixbuf) {
      const auto fitted = luciole::fitRasterPixels(pixbuf, 64, 10'000'000);
      TEST_CHECK(fitted && fitted->width == 64 && fitted->height == 48);
      const auto rows = packed(pixbuf);
      TEST_CHECK(fitted->rgba == resizeToRgba(rows.data(), 2001, 1500, ResizeSourceLayout::Rgbx, 64, 48));
      for (std::size_t i = 0; i < fitted->rgba.size(); ++i) {
        TEST_CHECK(i % 4 == 3 ? fitted->rgba[i] == 255 : std::abs(fitted->rgba[i] - plain->rgba[i]) <= 1);
      }
    };
    check(make(2001, 1500, true, 255).get());
    for (const int extra : {0, 12}) {
      for (const bool exact : {false, true}) {
        check(makeRows(2001, 1500, 4, extra, exact, kColors, kOpaque).pixbuf.get());
      }
    }
  }

  // The opaque and the straight-alpha resizer, given the same fully opaque picture, differ by rounding only.
  void theOpaqueAndStraightAlphaPathsAgreeOnAnOpaquePicture() {
    const auto pixbuf = make(2001, 1500, true, 255);
    const auto rows = packed(pixbuf.get());
    const auto opaque = resizeToRgba(rows.data(), 2001, 1500, ResizeSourceLayout::Rgbx, 64, 48);
    const auto straight = resizeToRgba(rows.data(), 2001, 1500, ResizeSourceLayout::Rgba, 64, 48);
    TEST_CHECK(opaque.size() == straight.size() && !opaque.empty());
    int worst = 0;
    for (std::size_t i = 0; i < opaque.size(); ++i) {
      worst = std::max(worst, std::abs(opaque[i] - straight[i]));
    }
    std::println("raster-fit: opaque and straight-alpha results differ by at most {} level(s)", worst);
    TEST_CHECK(worst <= 1);
  }

  // A quarter of the picture is transparent or half transparent, with colors far from the rest: the alpha-weighted
  // path keeps the colors of the opaque part, the opaque path would blend the colors of the hidden quarter into them.
  void aTranslucentRegionKeepsTheAlphaWeighting() {
    for (const int hidden : {0, 128}) {
      const Color color = [](int x, int y, int c) -> std::uint8_t {
        return x < 500 && y < 320 ? 255 : static_cast<std::uint8_t>(20 + c * 10 + (x + y) % 7);
      };
      const Alpha alpha = [hidden](int x, int y) -> std::uint8_t {
        return x < 500 && y < 320 ? static_cast<std::uint8_t>(hidden) : 255;
      };
      for (const int extra : {0, 12}) {
        for (const bool exact : {false, true}) {
          const auto picture = makeRows(1001, 640, 4, extra, exact, color, alpha);
          const auto fitted = luciole::fitRasterPixels(picture.pixbuf.get(), 128, 10'000'000);
          TEST_CHECK(fitted && fitted->width == 128 && fitted->height == 82);
          const auto rows = packed(picture.pixbuf.get());
          TEST_CHECK(fitted->rgba == resizeToRgba(rows.data(), 1001, 640, ResizeSourceLayout::Rgba, 128, 82));
          const auto opaque = resizeToRgba(rows.data(), 1001, 640, ResizeSourceLayout::Rgbx, 128, 82);
          int worstColor = 0;
          bool translucent = false;
          for (std::size_t i = 0; i < fitted->rgba.size(); ++i) {
            if (i % 4 == 3) {
              translucent = translucent || fitted->rgba[i] < 255;
            } else {
              worstColor = std::max(worstColor, std::abs(fitted->rgba[i] - opaque[i]));
            }
          }
          TEST_CHECK(translucent);
          TEST_CHECK(worstColor > 20);
        }
      }
    }
  }

#ifndef __SANITIZE_ADDRESS__
  // Bytes of private writable memory the process holds now.
  std::size_t dataBytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
      if (line.starts_with("VmData:")) {
        return std::stoull(line.substr(7)) * 1024;
      }
    }
    return 0;
  }
#endif

  // A 12 MP picture with padded rows is shrunk under a cap that leaves room for the result and the resizer's
  // scratch but not for a second copy of the picture.
  void aPaddedPictureIsShrunkWithoutAnotherCopyOfIt() {
#ifdef __SANITIZE_ADDRESS__
    std::println(
        "raster-fit: the memory cap check is skipped: a sanitizer build maps its shadow memory, which RLIMIT_DATA "
        "counts"
    );
    return;
#else
    const pid_t child = ::fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
      const auto pixbuf = make(4001, 3000, false, -1);
      const rlimit cap{dataBytes() + (12U << 20), dataBytes() + (12U << 20)};
      if (::setrlimit(RLIMIT_DATA, &cap) != 0) {
        ::_exit(3);
      }
      const auto fitted = luciole::fitRasterPixels(pixbuf.get(), 256, 20'000'000);
      ::_exit(fitted && fitted->width == 256 && fitted->height == 192 ? 0 : 2);
    }
    int status = 0;
    TEST_CHECK(::waitpid(child, &status, 0) == child);
    TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
  }
} // namespace

int main() {
  rowsWithPaddingGiveWhatPackedRowsGive();
  anOpaqueAlphaChannelIsShrunkAsOpaque();
  theOpaqueAndStraightAlphaPathsAgreeOnAnOpaquePicture();
  aTranslucentRegionKeepsTheAlphaWeighting();
  aPaddedPictureIsShrunkWithoutAnotherCopyOfIt();
  std::println("raster-fit: all checks passed");
  return 0;
}
