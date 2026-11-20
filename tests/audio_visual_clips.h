#pragma once

#include "gl_test_context.h"
#include "png_writer.h"
#include "render/core/renderer.h"
#include "test_check.h"
#include "ui/palette.h"
#include "workbench/color.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

// Sound files for the tests of the sound preview's visual, written byte by byte, and what the tests need beside
// them.
namespace test::audio {
  enum class SoundClip : std::uint8_t {
    ToneAndKicks, // a 440 Hz tone at half level with a decaying 55 Hz kick every half second
    Silence,
  };

  inline constexpr int kClipRate = 22050;

  // A mono 16-bit WAV of `seconds` at kClipRate.
  inline void writeSoundClip(const std::filesystem::path& path, SoundClip clip, int seconds) {
    const auto frames = static_cast<std::uint32_t>(kClipRate * seconds);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(44 + (frames * 2));
    const auto put = [&bytes](std::uint32_t value, int size) {
      for (int i = 0; i < size; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
      }
    };
    const auto tag = [&bytes](const char* text) { bytes.insert(bytes.end(), text, text + 4); };
    tag("RIFF");
    put(36 + (frames * 2), 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    put(1, 2); // PCM
    put(1, 2); // mono
    put(kClipRate, 4);
    put(kClipRate * 2, 4);
    put(2, 2);
    put(16, 2);
    tag("data");
    put(frames * 2, 4);
    for (std::uint32_t i = 0; i < frames; ++i) {
      const double t = static_cast<double>(i) / kClipRate;
      double value = 0;
      if (clip == SoundClip::ToneAndKicks) {
        const double sinceKick = std::fmod(t, 0.5);
        value = (0.35 * std::sin(2 * std::numbers::pi * 440 * t))
            + (0.6 * std::exp(-sinceKick * 12) * std::sin(2 * std::numbers::pi * 55 * sinceKick));
      }
      put(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32000))), 2);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(out.good());
  }

  // The default dark theme's roles, which the sound visuals and the media view draw with; a test process starts
  // with every role black.
  inline void useDarkTheme() {
    const luciole::workbench::ColorPalette roles;
    const auto color = [](luciole::workbench::Rgb rgb) {
      return rgba(static_cast<float>(rgb.r) / 255, static_cast<float>(rgb.g) / 255, static_cast<float>(rgb.b) / 255, 1);
    };
    Palette out{};
    out.primary = color(roles.primary);
    out.secondary = color(roles.secondary);
    out.tertiary = color(roles.tertiary);
    out.surface = color(roles.surface);
    out.onSurface = color(roles.onSurface);
    out.onSurfaceVariant = color(roles.onSurfaceVariant);
    out.surfaceContainerLow = color(roles.surfaceContainerLow);
    out.surfaceContainer = color(roles.surfaceContainer);
    out.surfaceContainerHigh = color(roles.surfaceContainerHigh);
    out.surfaceContainerHighest = color(roles.surfaceContainerHighest);
    out.outlineVariant = color(roles.outlineVariant);
    out.hover = out.surfaceContainerHigh;
    out.onHover = out.onSurface;
    setPalette(out);
  }

  // Writes `pixels` to LUCIOLE_CAPTURE_DIR as `name` when the variable is set.
  inline void keepIfAsked(const ::test::GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir != nullptr && dir[0] != '\0') {
      TEST_CHECK(::test::writePng(pixels, std::filesystem::path(dir) / name));
    }
  }

  // Only what Node::invalidateGpuResources needs: the texture manager of the context it runs in.
  class GpuResetRenderer final : public Renderer {
  public:
    explicit GpuResetRenderer(TextureManager& textures) : m_textures(textures) {}
    TextMetrics measureText(
        std::string_view /*text*/, float /*fontSize*/, const TextFont& /*font*/, float /*maxWidth*/, int /*maxLines*/,
        TextAlign /*align*/, TextEllipsize /*ellipsize*/, bool /*useMarkup*/
    ) override {
      return {};
    }
    TextMetrics measureFont(float /*fontSize*/, const TextFont& /*font*/) override { return {}; }
    void measureTextCursorStops(
        std::string_view /*text*/, float /*fontSize*/, const std::vector<std::size_t>& /*byteOffsets*/,
        std::vector<float>& outStops, const TextFont& /*font*/
    ) override {
      outStops.clear();
    }
    void measureTextCursorStopsWrapped(
        std::string_view /*text*/, float /*fontSize*/, const std::vector<std::size_t>& /*byteOffsets*/,
        float /*maxWidth*/, std::vector<TextCursorStop>& outStops, const TextFont& /*font*/
    ) override {
      outStops.clear();
    }
    TextMetrics measureGlyph(char32_t /*codepoint*/, float /*fontSize*/) override { return {}; }
    TextureManager& textureManager() override { return m_textures; }
    float renderScale() const noexcept override { return 1; }

  private:
    TextureManager& m_textures;
  };
} // namespace test::audio
