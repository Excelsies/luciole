#include "png_writer.h"
#include "render/core/color.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/spinner_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/shape_view.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <print>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using test::GlPixels;
  using test::SceneCapture;

  using Pixel = std::array<int, 4>;

  constexpr int kWidth = 240;
  constexpr int kHeight = 120;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};
  constexpr Pixel kBox{51, 102, 153, 255};
  constexpr Pixel kText{255, 204, 51, 255};
  constexpr Pixel kIcon{255, 255, 255, 255};
  constexpr Pixel kImage{255, 0, 255, 255};
  constexpr Pixel kClear{0, 0, 0, 0};
  constexpr Pixel kBluePixel{40, 60, 200, 255};
  constexpr Rgba8 kRed{220, 40, 40, 255};
  constexpr Rgba8 kGreen{40, 180, 80, 255};
  constexpr Rgba8 kWhite{240, 240, 240, 255};

  Color color(Pixel p) {
    return rgba(
        static_cast<float>(p[0]) / 255.0F, static_cast<float>(p[1]) / 255.0F, static_cast<float>(p[2]) / 255.0F,
        static_cast<float>(p[3]) / 255.0F
    );
  }

  Pixel pixel(Rgba8 c) { return {c.r, c.g, c.b, c.a}; }

  // A rounded box holding a label, a 2 by 2 image, an icon, and a shape view with a rectangle, a circle and a
  // sector, in logical pixels.
  struct Scene {
    ::Node root;
    Box* box = nullptr;
    Label* label = nullptr;
    Glyph* icon = nullptr;
    ShapeView* shapes = nullptr;

    explicit Scene(TextureId image) {
      box = static_cast<Box*>(root.addChild(std::make_unique<Box>()));
      box->setPosition(10, 10);
      box->setSize(150, 80);
      box->setFill(color(kBox));
      box->setRadius(12);
      label = static_cast<Label*>(box->addChild(std::make_unique<Label>()));
      label->setPosition(12, 12);
      label->setText("Capture");
      label->setFontSize(20);
      label->setColor(color(kText));
      icon = static_cast<Glyph*>(box->addChild(std::make_unique<Glyph>()));
      icon->setPosition(110, 46);
      TEST_CHECK(icon->setGlyph("folder"));
      icon->setGlyphSize(24);
      icon->setColor(color(kIcon));
      auto* picture = static_cast<ImageNode*>(root.addChild(std::make_unique<ImageNode>()));
      picture->setPosition(10, 98);
      picture->setSize(16, 16);
      picture->setTextureId(image);
      picture->setTextureSize(2, 2);
      shapes = static_cast<ShapeView*>(root.addChild(std::make_unique<ShapeView>([] { return 0U; })));
      shapes->setPosition(170, 10);
      shapes->setSize(60, 100);
      SceneShapes scene;
      auto rect = rectShape({1, 0}, 5, 5, 50, 20, 4);
      rect.fill = kRed;
      auto circle = circleShape({2, 0}, {30, 52}, 14);
      circle.fill = kGreen;
      auto sector = sectorShape({3, 0}, {30, 92}, 0, 14, -60, 60);
      sector.fill = kWhite;
      scene.shapes = {rect, circle, sector};
      shapes->setScene(std::move(scene));
    }
  };

  // The device pixel under logical point (x, y).
  std::array<std::uint8_t, 4> at(const GlPixels& pixels, float x, float y, float scale) {
    return pixels.at(static_cast<int>(std::floor(x * scale)), static_cast<int>(std::floor(y * scale)));
  }

  bool near(std::array<std::uint8_t, 4> actual, Pixel expected, int tolerance) {
    for (std::size_t i = 0; i < 4; ++i) {
      if (std::abs(static_cast<int>(actual[i]) - expected[i]) > tolerance) {
        std::println(
            stderr, "pixel {} {} {} {}, expected {} {} {} {}", actual[0], actual[1], actual[2], actual[3], expected[0],
            expected[1], expected[2], expected[3]
        );
        return false;
      }
    }
    return true;
  }

  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  // Device pixels fully inside the node's laid-out bounds.
  Region regionOf(const ::Node& node, float scale) {
    float x = 0;
    float y = 0;
    ::Node::absolutePosition(&node, x, y);
    return {
        .left = static_cast<int>(std::ceil(x * scale)),
        .top = static_cast<int>(std::ceil(y * scale)),
        .right = static_cast<int>(std::floor((x + node.width()) * scale)),
        .bottom = static_cast<int>(std::floor((y + node.height()) * scale)),
    };
  }

  struct Ink {
    int covered = 0; // at least half
    int partial = 0; // between a tenth and nine tenths
    int bare = 0;
  };

  // Every pixel of the region lies between `under` and `ink`, as coverage of `ink` over `under` gives.
  Ink inkOver(const GlPixels& pixels, Region region, Pixel under, Pixel ink) {
    Ink counts;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        const auto p = pixels.at(x, y);
        TEST_CHECK(p[3] == 255);
        const float t = static_cast<float>(p[0] - under[0]) / static_cast<float>(ink[0] - under[0]);
        for (std::size_t c = 0; c < 3; ++c) {
          const float expected = static_cast<float>(under[c]) + t * static_cast<float>(ink[c] - under[c]);
          TEST_NEAR(static_cast<float>(p[c]), expected, 6.0F);
        }
        counts.covered += t >= 0.5F ? 1 : 0;
        counts.partial += t > 0.1F && t < 0.9F ? 1 : 0;
        counts.bare += t <= 0.02F ? 1 : 0;
      }
    }
    return counts;
  }

  std::size_t differingBytes(const GlPixels& a, const GlPixels& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
      return std::numeric_limits<std::size_t>::max();
    }
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
      count += a.rgba[i] != b.rgba[i] ? 1 : 0;
    }
    return count;
  }

  // Partly covered pixels per pixel of ink for the label, and per device pixel of perimeter for the
  // circle. Edges drawn at the capture's scale are about one device pixel wide.
  struct Fringes {
    float text = 0;
    float circle = 0;
  };

  float circleFringe(const GlPixels& pixels, float scale) {
    int partial = 0;
    for (int y = static_cast<int>(36 * scale); y < static_cast<int>(88 * scale); ++y) {
      for (int x = static_cast<int>(174 * scale); x < static_cast<int>(226 * scale); ++x) {
        const int alpha = pixels.at(x, y)[3];
        partial += alpha > 25 && alpha < 230 ? 1 : 0;
      }
    }
    return static_cast<float>(partial) / (2 * std::numbers::pi_v<float> * 14 * scale);
  }

  Fringes sceneAtScale(const GlPixels& pixels, const Scene& scene, float scale) {
    TEST_CHECK(pixels.width == static_cast<int>(std::lround(kWidth * scale)));
    TEST_CHECK(pixels.height == static_cast<int>(std::lround(kHeight * scale)));
    TEST_CHECK(pixels.rgba.size() == static_cast<std::size_t>(pixels.width) * pixels.height * 4);

    // The box: its fill inside, nothing past its rounded corner or around it.
    TEST_CHECK(near(at(pixels, 100, 80, scale), kBox, 1));
    TEST_CHECK(near(at(pixels, 15, 85, scale), kBox, 1));
    TEST_CHECK(near(at(pixels, 11, 11, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 159.5F, 89.5F, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 5, 50, scale), kClear, 0));

    // The label: shaped text in its color over the box's fill.
    TEST_CHECK(scene.label->width() > 40);
    TEST_CHECK(scene.label->height() > 14);
    const auto text = inkOver(pixels, regionOf(*scene.label, scale), kBox, kText);
    TEST_CHECK(static_cast<float>(text.covered) > 60 * scale * scale);
    TEST_CHECK(text.bare > text.covered);

    // The icon: its glyph in its color over the box's fill.
    const auto icon = inkOver(pixels, regionOf(*scene.icon, scale), kBox, kIcon);
    TEST_CHECK(static_cast<float>(icon.covered) > 30 * scale * scale);
    TEST_CHECK(icon.bare > 0);

    // The image: its texture stretched over its bounds.
    TEST_CHECK(near(at(pixels, 18, 106, scale), kImage, 1));
    TEST_CHECK(near(at(pixels, 30, 106, scale), kClear, 0));

    // The shapes, at the shape view's position.
    TEST_CHECK(near(at(pixels, 200, 25, scale), pixel(kRed), 1));
    TEST_CHECK(near(at(pixels, 177, 17, scale), pixel(kRed), 1));
    TEST_CHECK(near(at(pixels, 200, 62, scale), pixel(kGreen), 1));
    TEST_CHECK(near(at(pixels, 200, 94, scale), pixel(kWhite), 1));
    TEST_CHECK(near(at(pixels, 200, 42, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 200, 107, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 172, 12, scale), kClear, 0));
    return {
        .text = static_cast<float>(text.partial) / static_cast<float>(text.covered),
        .circle = circleFringe(pixels, scale),
    };
  }

  void rejectsBadInput(SceneCapture& capture) {
    ::Node root;
    TEST_CHECK(capture.capture(root, 0, 10, 1).rgba.empty());
    TEST_CHECK(capture.capture(root, 10, -3, 1).rgba.empty());
    TEST_CHECK(capture.capture(root, 10, 10, 0).rgba.empty());
    TEST_CHECK(capture.capture(root, 10, 10, -1).rgba.empty());
    TEST_CHECK(capture.capture(root, 10, 10, std::nanf("")).rgba.empty());
    TEST_CHECK(capture.capture(root, 10, 10, std::numeric_limits<float>::infinity()).rgba.empty());
    const auto huge = capture.capture(root, 1 << 20, 10, 1);
    TEST_CHECK(huge.rgba.empty());
    TEST_CHECK(huge.width == 0);
    TEST_CHECK(huge.height == 0);
    TEST_CHECK(capture.capture(root, 10, 1 << 20, 1).rgba.empty());
    TEST_CHECK(root.width() == 0);
  }

  Box* addBox(::Node& parent, LayoutRect rect, Pixel fill) {
    auto* box = static_cast<Box*>(parent.addChild(std::make_unique<Box>()));
    box->setPosition(rect.x, rect.y);
    box->setSize(rect.width, rect.height);
    box->setFill(color(fill));
    return box;
  }

  ::Node* addGroup(::Node& parent, LayoutRect rect) {
    auto* group = parent.addChild(std::make_unique<::Node>());
    group->setPosition(rect.x, rect.y);
    group->setSize(rect.width, rect.height);
    return group;
  }

  // A node shifted by its translation, then scaled or rotated about its center, lands where that transform puts it.
  void addTransformed(::Node& root) {
    auto* grown = addBox(root, {.x = 100, .y = 5, .width = 10, .height = 10}, kBluePixel);
    grown->setTranslate(10, 0);
    grown->setScale(2);
    auto* turned = addBox(root, {.x = 128, .y = 17, .width = 24, .height = 6}, pixel(kGreen));
    turned->setRotation(std::numbers::pi_v<float> / 2);
  }

  void transformedAtScale(const GlPixels& pixels, float scale) {
    // The grown box covers 105 to 125 across and 0 to 20 down.
    TEST_CHECK(near(at(pixels, 106, 2, scale), kBluePixel, 1));
    TEST_CHECK(near(at(pixels, 122, 18, scale), kBluePixel, 1));
    TEST_CHECK(near(at(pixels, 103, 10, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 127, 10, scale), kClear, 0));
    // The turned bar stands from 137 to 143 across and 8 to 32 down.
    TEST_CHECK(near(at(pixels, 140, 11, scale), pixel(kGreen), 1));
    TEST_CHECK(near(at(pixels, 140, 29, scale), pixel(kGreen), 1));
    TEST_CHECK(near(at(pixels, 131, 20, scale), kClear, 0));
    TEST_CHECK(near(at(pixels, 149, 20, scale), kClear, 0));
  }

  // Paint order follows z order, opacity multiplies down the tree, a clipping node cuts its children, a hidden
  // node is not drawn, a proxy repaints its source at its own place, a spinner draws inside its bounds, and
  // transforms place their node.
  void paintFollowsTree(SceneCapture& capture) {
    ::Node root;
    auto* top = addBox(root, {.x = 0, .y = 0, .width = 20, .height = 20}, pixel(kRed));
    top->setZIndex(1);
    addBox(root, {.x = 10, .y = 0, .width = 20, .height = 20}, kBluePixel);
    auto* faded = addGroup(root, {.x = 40, .y = 0, .width = 20, .height = 20});
    faded->setOpacity(0.5F);
    addBox(*faded, {.x = 0, .y = 0, .width = 20, .height = 20}, kIcon);
    auto* clipper = addGroup(root, {.x = 70, .y = 0, .width = 10, .height = 10});
    clipper->setClipChildren(true);
    addBox(*clipper, {.x = 0, .y = 0, .width = 20, .height = 20}, pixel(kGreen));
    addBox(root, {.x = 0, .y = 25, .width = 10, .height = 10}, pixel(kRed))->setVisible(false);
    root.addChild(std::make_unique<RenderProxyNode>(top))->setPosition(40, 25);
    auto* spinner = static_cast<SpinnerNode*>(root.addChild(std::make_unique<SpinnerNode>()));
    spinner->setPosition(80, 22);
    spinner->setSize(16, 16);
    spinner->setColor(color(kIcon));
    spinner->setThickness(3);
    addTransformed(root);
    for (const float scale : kScales) {
      const auto pixels = capture.capture(root, 160, 40, scale);
      transformedAtScale(pixels, scale);
      TEST_CHECK(near(at(pixels, 15, 10, scale), pixel(kRed), 1));
      TEST_CHECK(near(at(pixels, 25, 10, scale), kBluePixel, 1));
      TEST_CHECK(near(at(pixels, 50, 10, scale), {128, 128, 128, 128}, 2));
      TEST_CHECK(near(at(pixels, 75, 5, scale), pixel(kGreen), 1));
      TEST_CHECK(near(at(pixels, 75, 15, scale), kClear, 0));
      TEST_CHECK(near(at(pixels, 85, 5, scale), kClear, 0));
      TEST_CHECK(near(at(pixels, 5, 30, scale), kClear, 0));
      TEST_CHECK(near(at(pixels, 45, 30, scale), pixel(kRed), 1));
      TEST_CHECK(near(at(pixels, 65, 30, scale), kClear, 0));
      int spun = 0;
      for (int y = static_cast<int>(22 * scale); y < static_cast<int>(38 * scale); ++y) {
        for (int x = static_cast<int>(80 * scale); x < static_cast<int>(96 * scale); ++x) {
          spun += pixels.at(x, y)[3] > 0 ? 1 : 0;
        }
      }
      TEST_CHECK(spun > 0);
    }
  }

  // LUCIOLE_CAPTURE_DIR names a folder that receives the captures for a person to look at.
  void keepForViewing(const GlPixels& pixels, float scale) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("scene-capture-{}x.png", scale);
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  std::vector<GlPixels> captureScene(SceneCapture& capture) {
    const std::array<std::uint8_t, 16> magenta{255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255};
    auto image = capture.textureManager().loadFromRgba(magenta.data(), 2, 2);
    TEST_CHECK(image.valid());
    std::vector<GlPixels> captures;
    std::vector<Fringes> fringes;
    {
      Scene scene(image.id);
      for (const float scale : kScales) {
        auto pixels = capture.capture(scene.root, kWidth, kHeight, scale);
        fringes.push_back(sceneAtScale(pixels, scene, scale));
        // Drawing the same scene again gives the same pixels.
        TEST_CHECK(differingBytes(capture.capture(scene.root, kWidth, kHeight, scale), pixels) == 0);
        captures.push_back(std::move(pixels));
      }
    }
    // Text and shapes are rasterized at the capture's scale, not drawn at one scale and stretched: the text's
    // fringe shrinks against its ink as the scale grows, and the circle's stays about one device pixel wide.
    TEST_CHECK(fringes[1].text < 0.85F * fringes[0].text);
    for (const auto& f : fringes) {
      TEST_CHECK(f.circle > 0.4F);
      TEST_CHECK(f.circle < 0.95F);
    }
    capture.textureManager().unload(image);
    return captures;
  }
} // namespace

int main() {
  std::vector<GlPixels> first;
  {
    std::string reason;
    const auto capture = SceneCapture::create(reason);
    if (!capture) {
      std::println("skipped: {}", reason);
      return test::kSkipExitCode;
    }
    // Rigs share one display, so a second one is refused while the first lives.
    std::string refused;
    TEST_CHECK(SceneCapture::create(refused) == nullptr);
    TEST_CHECK(refused == "another SceneCapture is alive");
    first = captureScene(*capture);
    paintFollowsTree(*capture);
    rejectsBadInput(*capture);
  }
  {
    // A second rig, with fresh caches, draws the same pixels.
    std::string reason;
    const auto capture = SceneCapture::create(reason);
    TEST_CHECK(capture != nullptr);
    const auto second = captureScene(*capture);
    TEST_CHECK(second.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
      TEST_CHECK(differingBytes(second[i], first[i]) == 0);
    }
  }
  for (std::size_t i = 0; i < first.size(); ++i) {
    keepForViewing(first[i], kScales[i]);
  }
  std::println("scene capture checks passed");
}
