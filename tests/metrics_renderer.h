#pragma once

#include "render/core/renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace test {
  // Measures text without fonts: each byte is half the font size wide and every line is one font size tall.
  class MetricsRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float size, const TextFont&, float width, int, TextAlign, TextEllipsize, bool
    ) override {
      const float natural = static_cast<float>(text.size()) * size * 0.5F;
      return {
          .width = width > 0 ? std::min(width, natural) : natural, .bottom = size, .capHeight = size, .lineCount = 1
      };
    }
    TextMetrics measureFont(float size, const TextFont&) override { return {.bottom = size, .capHeight = size}; }
    TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .bottom = size}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1; }
  };
} // namespace test
