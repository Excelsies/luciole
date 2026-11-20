#pragma once

#include "core/timer_manager.h"
#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/scene_shapes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <glib.h>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <utility>

// A Nest instrument view drawn offscreen, with the pixel probes its tests share.
namespace luciole::test::nest_pixels {
  using namespace luciole::workbench;
  using ::test::GlPixels;
  using ::test::SceneCapture;

  inline constexpr int kWidth = 900;
  inline constexpr int kHeight = 600;

  struct Rgb3 {
    double r = 0;
    double g = 0;
    double b = 0;
  };

  inline Rgb3 at(const GlPixels& pixels, double x, double y) {
    const int px = std::clamp(static_cast<int>(x), 0, pixels.width - 1);
    const int py = std::clamp(static_cast<int>(y), 0, pixels.height - 1);
    const auto p = pixels.at(px, py);
    return {.r = static_cast<double>(p[0]), .g = static_cast<double>(p[1]), .b = static_cast<double>(p[2])};
  }

  inline Rgb3 of(Rgb c) {
    return {.r = static_cast<double>(c.r), .g = static_cast<double>(c.g), .b = static_cast<double>(c.b)};
  }

  inline double distance(Rgb3 a, Rgb3 b) { return std::hypot(a.r - b.r, a.g - b.g, a.b - b.b); }

  inline double distance(Rgb3 a, Rgb b) { return distance(a, of(b)); }

  // An instrument view on the palette's surface, with its own input dispatcher and tweens.
  struct Stage {
    AnimationManager animations;
    ::Node root;
    Box* background = nullptr;
    InstrumentView* view = nullptr;
    InputDispatcher dispatcher;
    PartInputs inputs;
    int width = kWidth;
    int height = kHeight;

    explicit Stage(Tree tree, int w = kWidth, int h = kHeight) : width(w), height(h) {
      const ColorPalette palette;
      root.setAnimationManager(&animations);
      background = static_cast<Box*>(root.addChild(std::make_unique<Box>()));
      const auto& s = palette.surface;
      background->setFill(rgba(s.r / 255.0F, s.g / 255.0F, s.b / 255.0F, 1));
      background->setFrameSize(static_cast<float>(width), static_cast<float>(height));
      view = static_cast<InstrumentView*>(root.addChild(std::make_unique<InstrumentView>(WorkbenchInstrument::Nest, [] {
        return 0U;
      })));
      view->setFrameSize(static_cast<float>(width), static_cast<float>(height));
      dispatcher.setSceneRoot(&root);
      inputs = {
          .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = 1},
          .settings = {},
          .palette = {}
      };
    }

    const ShapeView& shapes() const { return view->shapeView(); }

    void key(std::uint32_t sym, std::uint32_t utf32 = 0) {
      dispatcher.setFocus(&view->shapeView().inputArea());
      dispatcher.keyEvent(sym, utf32, 0, true);
      dispatcher.keyEvent(sym, utf32, 0, false);
    }

    // Frames until the view shows its snapshot at rest and nothing more arrives, drawing each one.
    GlPixels settle(SceneCapture& capture, float scale) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      GlPixels pixels;
      for (int quiet = 0; quiet < 30;) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        bool delivered = false;
        while (g_main_context_iteration(nullptr, FALSE) != 0) {
          delivered = true;
        }
        TimerManager::instance().tick();
        animations.tick(16);
        view->setFrameSize(static_cast<float>(width), static_cast<float>(height));
        view->update(inputs, false);
        pixels = capture.capture(root, width, height, scale);
        const bool resting = view->shownSnapshot().serial == inputs.snapshot.serial && !animations.hasActive();
        quiet = resting && !delivered ? quiet + 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return pixels;
    }
  };

  inline const Shape* pieceShape(const ShapeView& view, NodeId node, SceneRole role = SceneRole::Piece) {
    const auto found = std::ranges::find_if(view.shapes(), [&](const Shape& shape) {
      return shape.hittable && shape.id.node == node && sceneRole(shape.id.role) == role;
    });
    return found == view.shapes().end() ? nullptr : &*found;
  }

  inline void keep(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }
} // namespace luciole::test::nest_pixels
