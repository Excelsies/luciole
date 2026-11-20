#pragma once

#include "gl_test_context.h"

#include <filesystem>

namespace test {
  // Writes `pixels` as an 8-bit RGBA PNG into a new file beside `path`, then renames it over `path`, so the file
  // there is replaced whole or not at all. False, leaving `path` as it was and no file behind, when the pixels are
  // empty or do not match their size, when `path` holds anything but a regular file its owner may write, or when
  // the file cannot be written.
  [[nodiscard]] bool writePng(const GlPixels& pixels, const std::filesystem::path& path);
} // namespace test
