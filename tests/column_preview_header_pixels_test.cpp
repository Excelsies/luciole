#include "png_writer.h"
#include "scene_capture.h"
#include "test_check.h"
#include "text_tile_view_rig.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>

// The column preview's header drawn offscreen with real text at the narrowest widths, for a file, a sound and a file
// being edited with unsaved changes. The images are kept when LUCIOLE_CAPTURE_DIR names a folder.
namespace {
  using luciole::ColumnPreviewEditBar;
  using luciole::ColumnPreviewView;

  constexpr int kHeight = 220;
  constexpr float kListText = 13;

  struct Look {
    const char* name;
    bool media;
    ColumnPreviewEditBar bar;
  };

  void drawn(::test::SceneCapture& capture, const Look& look, int width) {
    ColumnPreviewView view;
    view.setTextSize(luciole::quietTextSize(kListText, 0));
    view.setFile("recording of the weekly meeting.wav", "WAV audio");
    view.setFacts("344.6 KiB · 2026-11-19 19:06");
    view.setSizeAvailable(!look.media, "Video and sound are always fitted to the pane.");
    view.setEditBar(look.bar);
    const auto pixels = capture.capture(view, width, kHeight, 1);
    TEST_CHECK(pixels.width == width && pixels.height == kHeight);
    // The view fills its capture: nothing of the header is laid out past its right edge.
    for (const Node* part : std::array<const Node*, 3>{&view.openButton(), &view.nameLabel(), &view.largerButton()}) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(part, x, y);
      TEST_CHECK(x >= 0 && x + part->width() <= static_cast<float>(width) + 0.01F);
    }
    if (const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR"); dir != nullptr && dir[0] != '\0') {
      const auto path = std::filesystem::path(dir) / std::format("header_{}_{}.png", look.name, width);
      TEST_CHECK(::test::writePng(pixels, path));
      std::println("wrote {}", path.string());
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
  const std::array<Look, 3> looks{{
      {"file", false, {.shown = true, .enableAvailable = true}},
      {"sound", true, {}},
      {"editing", false, {.shown = true, .editing = true, .saveAvailable = true, .unsaved = true}},
  }};
  for (const auto& look : looks) {
    for (const int width : {140, 165, 185}) {
      drawn(*capture, look, width);
    }
  }
  std::println("column-preview-header-pixels: all checks passed");
  return 0;
}
