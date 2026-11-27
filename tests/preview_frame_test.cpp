#include "png_writer.h"
#include "render/core/image_decoder.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/flex.h"
#include "ui/controls/preview_frame.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>

// The frame the Workbench draws around its pictures: rounded corners, the outline and the soft shadow of the side
// preview, also for a picture the box cuts.
namespace {
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;
  using Pixel = std::array<std::uint8_t, 4>;

  constexpr int kBox = 120;
  constexpr int kBoxHeight = 90;
  constexpr int kMargin = 24; // beyond kPreviewImageInset, so the shadow is whole
  constexpr int kScene = kBox + (2 * kMargin);
  constexpr int kSceneHeight = kBoxHeight + (2 * kMargin);

  // Left half red, right half blue, with a one-pixel white column in the middle, so a shifted picture shows.
  std::shared_ptr<const LoadedImageFile> halves(int width, int height) {
    auto pixels = std::make_shared<LoadedImageFile>();
    pixels->width = width;
    pixels->height = height;
    pixels->sourceWidth = width;
    pixels->sourceHeight = height;
    pixels->rgba.resize(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto offset = static_cast<std::size_t>((y * width + x) * 4);
        const bool middle = x == width / 2;
        const bool left = x < width / 2;
        pixels->rgba[offset] = middle || left ? 255 : 0;
        pixels->rgba[offset + 1] = middle ? 255 : 0;
        pixels->rgba[offset + 2] = middle || !left ? 255 : 0;
        pixels->rgba[offset + 3] = 255;
      }
    }
    return pixels;
  }

  void keep(const ::test::GlPixels& pixels, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("preview-frame-{}.png", name);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  std::unique_ptr<Flex> stage() {
    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Start);
    root->setPadding(kMargin);
    root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    return root;
  }

  struct Picture {
    luciole::PreviewFrame* frame = nullptr;
    TextureHandle texture{};
  };

  // A PreviewFrame in a box of kBox by kBoxHeight showing a `width` by `height` picture at x, y within it.
  std::unique_ptr<Flex>
  framed(::test::SceneCapture& capture, Picture& picture, float x, float y, float width, float height) {
    auto root = stage();
    auto frame = std::make_unique<luciole::PreviewFrame>();
    picture.frame = frame.get();
    const auto pixels = halves(32, 24);
    picture.texture = capture.textureManager().loadFromPixels(
        pixels->rgba.data(), pixels->width, pixels->height, TextureDataFormat::Rgba
    );
    frame->image().setTextureId(picture.texture.id);
    frame->image().setTextureSize(pixels->width, pixels->height);

    frame->setPicture(x, y, width, height);
    frame->setFlexGrow(1);
    auto slot = std::make_unique<Flex>();
    slot->setDirection(FlexDirection::Vertical);
    slot->setAlign(FlexAlign::Stretch);
    slot->setMinWidth(kBox);
    slot->setMaxWidth(kBox);
    slot->setMinHeight(kBoxHeight);
    slot->setMaxHeight(kBoxHeight);
    slot->addChild(std::move(frame));
    root->addChild(std::move(slot));
    return root;
  }

  ::test::GlPixels draw(::test::SceneCapture& capture, Node& root) {
    // Two frames: the first lays out and uploads, the second draws what it settled.
    (void)capture.capture(root, kScene, kSceneHeight, 1);
    return capture.capture(root, kScene, kSceneHeight, 1);
  }

  // The edge pixel is the outline blended over what lies under it: nearer the outline than the picture or the surface.
  bool onOutline(const Pixel& pixel, const Pixel& picture) {
    const auto outline = bytesOf(palette.outlineVariant);
    const auto surface = bytesOf(palette.surfaceContainerLow);
    return distance(pixel, outline) < distance(pixel, picture) && distance(pixel, outline) < distance(pixel, surface);
  }

  // Rounded corners show the surface, the outline sits on the edge, the shadow falls below only while enabled.
  void stylesTheFittedPicture(::test::SceneCapture& capture) {
    const auto surface = bytesOf(palette.surfaceContainerLow);
    for (const bool shadows : {true, false}) {
      Style::setPopupShadowsEnabled(shadows);
      Picture picture;
      auto root = framed(capture, picture, 0, 0, kBox, kBoxHeight);
      const auto pixels = draw(capture, *root);
      keep(pixels, std::format("fitted-shadow-{}", shadows));
      TEST_CHECK(!picture.frame->cut());
      TEST_CHECK(distance(pixels.at(kMargin, kMargin), surface) < 24); // the rounded corner is not the picture
      TEST_CHECK(distance(pixels.at(kMargin + 20, kMargin + kBoxHeight / 2), {255, 0, 0, 255}) < 40);
      TEST_CHECK(onOutline(pixels.at(kMargin + 30, kMargin), {255, 0, 0, 255}));
      TEST_CHECK(distance(pixels.at(kMargin + kBox / 2 + 30, kMargin + kBoxHeight / 2), {0, 0, 255, 255}) < 40);
      const auto below = pixels.at(kMargin + kBox / 2, kMargin + kBoxHeight + 3);
      if (shadows) {
        TEST_CHECK(distance(below, surface) > 6); // darker than the surface
      } else {
        TEST_CHECK(distance(below, surface) == 0);
      }
      capture.textureManager().unload(picture.texture);
    }
    Style::setPopupShadowsEnabled(true);
  }

  // A picture smaller than its box is framed at its own size, the box's corner stays bare.
  void framesTinyPictures(::test::SceneCapture& capture) {
    Picture picture;
    auto root = framed(capture, picture, 56, 40, 8, 6);
    const auto pixels = draw(capture, *root);
    keep(pixels, "tiny");
    const auto surface = bytesOf(palette.surfaceContainerLow);
    TEST_CHECK(!picture.frame->cut());
    TEST_CHECK(distance(pixels.at(kMargin + 1, kMargin + 1), surface) == 0);
    TEST_CHECK(distance(pixels.at(kMargin + 56 + 6, kMargin + 40 + 3), {0, 0, 255, 255}) < 80);
    capture.textureManager().unload(picture.texture);
  }

  // A picture larger than the box is clipped to it and rounded at the box's corners, not its own.
  void roundsTheVisibleFrameOfAZoomedPicture(::test::SceneCapture& capture) {
    const auto surface = bytesOf(palette.surfaceContainerLow);
    for (const float panX : {0.0F, -90.0F}) {
      Picture picture;
      auto root = framed(capture, picture, panX, -30, 240, 180);
      const auto pixels = draw(capture, *root);
      keep(pixels, std::format("zoomed-pan-{}", static_cast<int>(panX)));
      TEST_CHECK(picture.frame->cut());
      TEST_CHECK(picture.frame->visible().width == kBox && picture.frame->visible().height == kBoxHeight);
      // The box's corners are rounded: bare surface at the very corner pixel.
      for (const auto& [x, y] :
           {std::pair{kMargin, kMargin}, std::pair{kMargin + kBox - 1, kMargin},
            std::pair{kMargin, kMargin + kBoxHeight - 1}, std::pair{kMargin + kBox - 1, kMargin + kBoxHeight - 1}}) {
        TEST_CHECK(distance(pixels.at(x, y), surface) < 30);
      }
      // The outline runs along the box's edge, over the picture.
      TEST_CHECK(onOutline(pixels.at(kMargin + 40, kMargin), {255, 0, 0, 255}));
      TEST_CHECK(onOutline(pixels.at(kMargin, kMargin + 40), {255, 0, 0, 255}));
      // Inside is the picture, outside the box (in the margin) is not.
      const auto inside = pixels.at(kMargin + 60, kMargin + 45);
      TEST_CHECK(distance(inside, surface) > 100);
      for (const auto& [x, y] :
           {std::pair{kMargin - 4, kMargin + 40}, std::pair{kMargin + kBox + 3, kMargin + 40},
            std::pair{kMargin + 40, kMargin - 4}}) {
        const auto outside = pixels.at(x, y);
        TEST_CHECK(distance(outside, {255, 0, 0, 255}) > 150 && distance(outside, {0, 0, 255, 255}) > 150);
      }
      capture.textureManager().unload(picture.texture);
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  stylesTheFittedPicture(*capture);
  framesTinyPictures(*capture);
  roundsTheVisibleFrameOfAZoomedPicture(*capture);
  std::println("preview-frame: all checks passed");
  return 0;
}
