#include "imaging/raster_decoder.h"
#include "render/core/image_resize.h"
#include "test_check.h"

#include <cstring>
#include <fstream>
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

  // Colors that differ from pixel to pixel.
  std::uint8_t channel(int x, int y, int c) {
    const std::uint32_t noise = (static_cast<std::uint32_t>(x) * 374761393U
                                 + static_cast<std::uint32_t>(y) * 668265263U
                                 + static_cast<std::uint32_t>(c) * 2147483647U)
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

  // Rows padded to four bytes are what gdk_pixbuf_new gives for a width that is not a multiple of four.
  void rowsWithPaddingGiveWhatPackedRowsGive() {
    for (const bool hasAlpha : {false, true}) {
      const auto pixbuf = make(1001, 640, hasAlpha, -1);
      TEST_CHECK(gdk_pixbuf_get_rowstride(pixbuf.get()) != 1001 * gdk_pixbuf_get_n_channels(pixbuf.get()) || hasAlpha);
      const auto fitted = luciole::fitRasterPixels(pixbuf.get(), 128, 10'000'000);
      TEST_CHECK(fitted && fitted->width == 128 && fitted->height == 82);
      const auto rows = packed(pixbuf.get());
      const auto expected =
          resizeToRgba(rows.data(), 1001, 640, hasAlpha ? ResizeSourceLayout::Rgba : ResizeSourceLayout::Rgb, 128, 82);
      TEST_CHECK(fitted->rgba == expected);
    }
  }

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

  // A 12 MP picture with padded rows is shrunk under a cap that leaves room for the result and the resizer's
  // scratch but not for a second copy of the picture.
  void aPaddedPictureIsShrunkWithoutAnotherCopyOfIt() {
#ifdef __SANITIZE_ADDRESS__
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
  aPaddedPictureIsShrunkWithoutAnotherCopyOfIt();
  std::println("raster-fit: all checks passed");
  return 0;
}
