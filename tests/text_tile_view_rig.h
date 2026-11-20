#pragma once

#include "core/timer_manager.h"
#include "imaging/text_document_loader.h"
#include "imaging/text_tile_service.h"
#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/scrollbar.h"
#include "ui/controls/text_tile_view.h"
#include "ui/palette.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <glib.h>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// A text tile view as the root of a scene drawn offscreen, with a real tile service, the default main context, the
// timer manager and an input dispatcher.
namespace luciole::test::tiles {
  using Clock = std::chrono::steady_clock;

  // Dark roles, each a color no other role shares.
  inline Palette testPalette() {
    Palette out{};
    out.primary = rgbHex(0xF0C040);
    out.onPrimary = rgbHex(0x101010);
    out.secondary = rgbHex(0x60A0F0);
    out.onSecondary = rgbHex(0x101010);
    out.tertiary = rgbHex(0x60E0A0);
    out.onTertiary = rgbHex(0x101010);
    out.error = rgbHex(0xF06080);
    out.onError = rgbHex(0x101010);
    out.surface = rgbHex(0x141418);
    out.onSurface = rgbHex(0xE8E8F0);
    out.onSurfaceVariant = rgbHex(0x9898A8);
    out.surfaceContainerLow = rgbHex(0x1C1C24);
    out.surfaceContainer = rgbHex(0x24242C);
    out.surfaceContainerHigh = rgbHex(0x30303C);
    out.surfaceContainerHighest = rgbHex(0x3C3C48);
    out.outlineVariant = rgbHex(0x58586C);
    out.primaryContainer = rgbHex(0x3C3C48);
    out.onPrimaryContainer = rgbHex(0xE8E8F0);
    out.secondaryContainer = rgbHex(0x30303C);
    out.onSecondaryContainer = rgbHex(0xE8E8F0);
    out.hover = rgbHex(0x30303C);
    out.onHover = rgbHex(0xE8E8F0);
    out.shadow = rgbHex(0x000000);
    out.scrim = rgbHex(0x000000);
    return out;
  }

  inline std::string cssLines(int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
      out += std::format(".rule{} {{ color: #{:02x}{:02x}80; }}\n", i, (i * 7) % 256, (i * 13) % 256);
    }
    return out;
  }

  // The document a loader would give for these bytes and this name.
  inline TextDocument textOf(std::string bytes, std::string_view name) {
    return textDocument(UserFileContent{.bytes = std::move(bytes), .truncated = false, .identity = {}}, name, {});
  }

  using Pixel = std::array<std::uint8_t, 4>;

  inline Pixel bytesOf(const Color& color) {
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0F)); };
    return {byte(color.r), byte(color.g), byte(color.b), 255};
  }

  inline int distance(const Pixel& a, const Pixel& b) {
    return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
  }

  // Pixels other than the background in rows [top, bottom] and columns [left, right).
  inline std::size_t inkIn(const ::test::GlPixels& pixels, int left, int right, int top, int bottom) {
    const auto background = bytesOf(palette.surfaceContainerLow);
    std::size_t count = 0;
    for (int y = std::max(0, top); y <= std::min(bottom, pixels.height - 1); ++y) {
      for (int x = std::max(0, left); x < std::min(right, pixels.width); ++x) {
        count += distance(pixels.at(x, y), background) > 60 ? 1 : 0;
      }
    }
    return count;
  }

  // Writes the pixels as text-tile-<name>-<scale>x.png when LUCIOLE_CAPTURE_DIR is set.
  inline void keep(const ::test::GlPixels& pixels, std::string_view name, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("text-tile-{}-{}x.png", name, scale);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  struct Rig {
    ::test::SceneCapture& capture;
    TextTileService service;
    std::unique_ptr<TextTileView> view;
    InputDispatcher dispatcher;
    int width = 400;
    int height = 300;
    float scale = 1;
    std::size_t framesAsked = 0;
    std::uint32_t serial = 0;

    explicit Rig(::test::SceneCapture& rig, std::size_t budget = TextTileView::kDefaultTextureBudget)
        : capture(rig), view(std::make_unique<TextTileView>(service, budget)) {
      view->setTextSize(12);
      view->onFrameNeeded = [this] { ++framesAsked; };
      dispatcher.setSceneRoot(view.get());
    }

    // One frame: main-context work and due timers, the view's update, then layout and drawing.
    ::test::GlPixels frame() {
      while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
      }
      TimerManager::instance().tick();
      view->update();
      return capture.capture(*view, width, height, scale);
    }

    // Frames until the view has settled; fails after ten seconds.
    ::test::GlPixels settle() {
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      for (;;) {
        auto pixels = frame();
        if (view->settled()) {
          return pixels;
        }
        TEST_CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    // Frames for a while without asking for anything to settle.
    void idle(std::chrono::milliseconds span) {
      const auto end = Clock::now() + span;
      while (Clock::now() < end) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }

    bool wheel(float x, float y, bool vertical, int notches) {
      dispatcher.pointerEnter(x, y, ++serial);
      return dispatcher.pointerAxis(x, y, vertical ? 0 : 1, 0, 10.0 * notches, notches, 120 * notches, 0);
    }
  };
} // namespace luciole::test::tiles
