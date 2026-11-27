#include "column_preview_fixture.h"
#include "column_preview_rig.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/text_tile_view.h"
#include "ui/palette.h"

#include <print>
#include <string>
#include <thread>

// The hex dump of a binary file in the Workbench preview, inside the page card, at the widths a column has.
namespace {
  using luciole::test::preview::Rig;

  // The dump the view shows at a column of `width`, settled.
  const TextTileView& settledDump(Rig& rig, int width, float scale) {
    rig.show("blob.bin");
    (void)rig.settle(width, 420, scale);
    const auto* hex = dynamic_cast<const TextTileView*>(rig.view->content());
    TEST_CHECK(hex != nullptr);
    return *hex;
  }

  // Rows that fit the view show whole, with no bar sideways. A column of 260, the Workbench's first, and every
  // width from there up, the narrow band between one dump's width and the next's included, draws whole rows; at
  // 190 and 140 nothing fits and the bar shows.
  void rowsAreWholeWhereTheyFit(::test::SceneCapture& capture, const luciole::test::preview::PreviewFiles& files) {
    Rig rig(capture, files);
    for (const float scale : {1.0F, 1.5F}) {
      for (const int width : {260, 280, 300, 349, 350, 355, 360, 365, 366, 372, 400, 460, 500, 640}) {
        const auto& hex = settledDump(rig, width, scale);
        std::println("{}x, column {}: view {} wide, sideways {}", scale, width, hex.width(), hex.maxScrollX());
        TEST_CHECK(hex.maxScrollX() == 0 && !hex.horizontalBar().Node::visible());
      }
      for (const int width : {140, 190}) {
        const auto& hex = settledDump(rig, width, scale);
        TEST_CHECK(hex.maxScrollX() > 0 && hex.horizontalBar().Node::visible());
      }
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
  static const luciole::test::preview::PreviewFiles files;
  rowsAreWholeWhereTheyFit(*capture, files);
  std::println("column-preview-hex: all checks passed");
  return 0;
}
