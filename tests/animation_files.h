#pragma once

#include "temp_folder.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace test {
  // One frame of a single color, straight alpha, or of two colors split at half its height.
  struct FlatFrame {
    std::array<std::uint8_t, 4> rgba{};
    int delayMs = 100;
    std::optional<std::array<std::uint8_t, 4>> lower{}; // the lower half's color; APNG only
  };

  // A GIF89a of opaque flat frames. `loopExtension` writes the NETSCAPE2.0 loop count; none plays once.
  std::vector<std::uint8_t>
  animatedGif(int width, int height, const std::vector<FlatFrame>& frames, std::optional<std::uint16_t> loopExtension);

  // An APNG of flat frames with alpha, played `plays` times (0 forever).
  std::vector<std::uint8_t>
  animatedPng(int width, int height, const std::vector<FlatFrame>& frames, std::uint32_t plays);

  // An animated lossless WebP of flat frames with alpha; `loopCount` 0 plays forever.
  std::vector<std::uint8_t>
  animatedWebp(int width, int height, const std::vector<FlatFrame>& frames, std::uint16_t loopCount);

  // A plain PNG of one flat color: no acTL chunk.
  std::vector<std::uint8_t> stillPng(int width, int height, const std::array<std::uint8_t, 4>& rgba);

  // A lossless WebP of one flat color: a bare VP8L chunk, or with `extended` a VP8X header without the animation
  // flag before it.
  std::vector<std::uint8_t> stillWebp(int width, int height, const std::array<std::uint8_t, 4>& rgba, bool extended);

  // Writes `bytes` to a new file at `path`; false when it cannot be written whole.
  [[nodiscard]] bool writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

  // A private directory under the system temporary folder, removed with everything in it when this goes.
  struct TemporaryDirectory : luciole::test::TempFolder {
    TemporaryDirectory() : TempFolder("luciole-animation") {}
  };
} // namespace test
