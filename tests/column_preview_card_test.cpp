#include "column_preview_fixture.h"
#include "column_preview_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/sheet_preview.h"
#include "thumbnails/thumbnail_worker.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <array>
#include <cmath>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The page card around a text, a table and an archive's listing draws its outline whole: on all four sides, over the
// whole length of each side, at every column width and pixel scale.
namespace {
  using luciole::test::preview::Rig;
  using luciole::test::tiles::bytesOf;
  using luciole::test::tiles::distance;

  constexpr int kHeight = 420;

  // The pixels along each side of the card, past its corners, are the outline.
  void outlineIsWhole(const ::test::GlPixels& pixels, const Node& card, float scale, std::string_view what) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&card, x, y);
    const auto outline = bytesOf(palette.outlineVariant);
    const int left = static_cast<int>(std::floor(x * scale));
    const int top = static_cast<int>(std::floor(y * scale));
    const int right = static_cast<int>(std::floor((x + card.width()) * scale)) - 1;
    const int bottom = static_cast<int>(std::floor((y + card.height()) * scale)) - 1;
    const int corner = static_cast<int>(std::ceil((Style::radiusXl + 2.0F) * scale));
    TEST_CHECK(right - left > 4 * corner && bottom - top > 2 * corner);
    // Pixels missing along the top, bottom, left and right.
    std::array<int, 4> missing{};
    // The outline is Style::borderWidth wide, which at a fractional scale or position blends into two pixels: one of
    // the two at the edge is the outline's color.
    const auto on = [&](int side, int px, int py, int inwardX, int inwardY) {
      const bool found =
          distance(pixels.at(px, py), outline) < 30 || distance(pixels.at(px + inwardX, py + inwardY), outline) < 30;
      missing.at(static_cast<std::size_t>(side)) += found ? 0 : 1;
    };
    for (int column = left + corner; column < right - corner; ++column) {
      on(0, column, top, 0, 1);
      on(1, column, bottom, 0, -1);
    }
    for (int row = top + corner; row < bottom - corner; ++row) {
      on(2, left, row, 1, 0);
      on(3, right, row, -1, 0);
    }
    std::println(
        "{} at {}x: outline pixels missing: top {}, bottom {}, left {}, right {}", what, scale, missing[0], missing[1],
        missing[2], missing[3]
    );
    TEST_CHECK((missing == std::array<int, 4>{}));
  }

  void cardsHaveWholeOutlines(Rig& rig) {
    for (const auto* file : {"style.css", "data.csv", "pack.zip", "blob.bin"}) {
      rig.show(file);
      for (const int width : {190, 262, 460}) {
        for (const float scale : {1.0F, 1.5F}) {
          const auto pixels = rig.settle(width, kHeight, scale);
          const Node* card = rig.view->content()->parent();
          TEST_CHECK(card != nullptr);
          outlineIsWhole(pixels, *card, scale, std::string(file) + " in " + std::to_string(width));
        }
      }
    }
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--sheet-preview-worker") {
    return luciole::runSheetPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--mesh-preview-worker") {
    return luciole::runMeshPreviewWorker(std::span(args).subspan(1));
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  if (!args.empty()) {
    return 2;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(luciole::test::tiles::testPalette());
  static const luciole::test::preview::PreviewFiles files;
  Rig rig(*capture, files);
  cardsHaveWholeOutlines(rig);
  std::println("column-preview-card: all checks passed");
  return 0;
}
