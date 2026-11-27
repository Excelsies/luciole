#include "test_check.h"
#include "ui/workbench/column_preview_layout.h"

#include <print>

// The picture's box in a Workbench preview slot and the pixels asked for it agree: the texture holds at least as
// many pixels as the box shows at the output scale. Worked examples are computed by hand from the slot's width less
// 24 px of padding, an 11 px scrollbar and its gap, and a height limit of the slot's height less 250.
namespace {
  using luciole::columnImageLayout;

  void aPortraitPageIsRequestedAtItsDrawnHeight() {
    const auto page = columnImageLayout(595, 842, 460, 700, 1);
    TEST_CHECK(page.width == 425 && page.height == 450);
    TEST_CHECK(page.pixels >= 450 && page.pixels <= 452);
    const auto tall = columnImageLayout(595, 842, 460, 1200, 1);
    TEST_CHECK(tall.height > 600 && tall.height < 602);
    TEST_CHECK(tall.pixels >= 601 && tall.pixels <= 603);
    const auto sharp = columnImageLayout(595, 842, 460, 1200, 2);
    TEST_CHECK(sharp.pixels >= 1202 && sharp.pixels <= 1204);
    const auto strip = columnImageLayout(1000, 3000, 320, 900, 1);
    TEST_CHECK(strip.width == 285 && strip.height == 650);
    TEST_CHECK(strip.pixels >= 650 && strip.pixels <= 652);
  }

  void aLandscapePictureKeepsItsWidth() {
    const auto wide = columnImageLayout(1600, 900, 460, 700, 1);
    TEST_CHECK(wide.width == 425 && wide.pixels >= 425 && wide.pixels <= 427);
    const auto unknown = columnImageLayout(0, 0, 460, 700, 1);
    TEST_CHECK(unknown.height == 96);
  }

  void theRequestNeverExceedsTheSideLimit() {
    const auto huge = columnImageLayout(100000, 100000, 4000, 4000, 4);
    TEST_CHECK(huge.pixels == luciole::kMaxPreviewImageSide);
  }
} // namespace

int main() {
  aPortraitPageIsRequestedAtItsDrawnHeight();
  aLandscapePictureKeepsItsWidth();
  theRequestNeverExceedsTheSideLimit();
  std::println("column-preview-layout: all checks passed");
  return 0;
}
