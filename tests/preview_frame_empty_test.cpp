#include "animation_files.h"
#include "png_writer.h"
#include "render/scene/image_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/animated_image_view.h"
#include "ui/controls/flex.h"
#include "ui/controls/preview_frame.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// A picture frame with no picture yet draws nothing: no shadow, outline or corner mask around an empty box.
namespace {
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;

  constexpr int kWidth = 200;
  constexpr int kHeight = 160;

  std::unique_ptr<Flex> stage() {
    auto root = std::make_unique<Flex>();
    root->setDirection(FlexDirection::Vertical);
    root->setAlign(FlexAlign::Stretch);
    root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
    return root;
  }

  void keep(const ::test::GlPixels& pixels, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("preview-frame-empty-{}.png", name);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Every pixel is the surface: no shadow, outline or mask is drawn.
  void allBlank(const ::test::GlPixels& pixels, std::string_view what) {
    std::println("checking {}", what);
    const auto surface = bytesOf(palette.surfaceContainerLow);
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        TEST_CHECK(distance(pixels.at(x, y), surface) < 3);
      }
    }
  }

  void framesWithoutATextureDrawNothing(::test::SceneCapture& capture) {
    Style::setPopupShadowsEnabled(true);
    auto root = stage();
    auto frame = std::make_unique<luciole::PreviewFrame>(ColorRole::SurfaceContainerLow);
    frame->setPicture(0, 0, 120, 90);
    frame->setFlexGrow(1);
    auto* shown = frame.get();
    root->addChild(std::move(frame));
    (void)capture.capture(*root, kWidth, kHeight, 1);
    const auto pixels = capture.capture(*root, kWidth, kHeight, 1);
    keep(pixels, "no-texture");
    TEST_CHECK(!shown->image().textureId().valid());
    allBlank(pixels, "a frame without a texture");
  }

  void animationsBeforeTheirFirstFrameDrawNothing(::test::SceneCapture& capture) {
    Style::setPopupShadowsEnabled(true);
    ::test::TemporaryDirectory dir;
    const auto path = dir.path() / "first.gif";
    const std::vector<::test::FlatFrame> frames{{{255, 0, 0, 255}, 20}, {{0, 255, 0, 255}, 30}};
    TEST_CHECK(::test::writeBytes(path, ::test::animatedGif(160, 90, frames, 0)));
    auto root = stage();
    auto view = std::make_unique<AnimatedImageView>(ColorRole::SurfaceContainerLow);
    view->setFlexGrow(1);
    auto* shown = view.get();
    root->addChild(std::move(view));
    shown->setFile(luciole::Location::fromPath(path.string()));
    (void)capture.capture(*root, kWidth, kHeight, 1);
    const auto pixels = capture.capture(*root, kWidth, kHeight, 1);
    keep(pixels, "animation");
    TEST_CHECK(!shown->pictureShown());
    allBlank(pixels, "an animation before its first frame");
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
  animationsBeforeTheirFirstFrameDrawNothing(*capture);
  framesWithoutATextureDrawNothing(*capture);
  std::println("preview-frame-empty: all checks passed");
  return 0;
}
