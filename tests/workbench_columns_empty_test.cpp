#include "png_writer.h"
#include "scene_capture.h"
#include "ui/palette.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/scene_style.h"
#include "workbench_view_rig.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

// The columns ahead of a folder with no subfolders and no file under the cursor: the first says so in one sentence,
// the others draw nothing. A folder with subfolders lists them again.
namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  using ::test::GlPixels;

  constexpr int kWidth = 1900;
  constexpr int kHeight = 900;

  Color color(workbench::Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  // The default dark palette, as the scene builders keep its roles.
  Palette darkPalette() {
    const workbench::ColorPalette roles;
    Palette out{};
    out.surface = color(roles.surface);
    out.onSurface = color(roles.onSurface);
    out.onSurfaceVariant = color(roles.onSurfaceVariant);
    out.surfaceContainerLow = color(roles.surfaceContainerLow);
    out.surfaceContainer = color(roles.surfaceContainer);
    out.surfaceContainerHigh = color(roles.surfaceContainerHigh);
    out.surfaceContainerHighest = color(roles.surfaceContainerHighest);
    out.outlineVariant = color(roles.outlineVariant);
    out.primary = color(roles.primary);
    out.onPrimary = color(roles.surface);
    out.primaryContainer = color(roles.surfaceContainerHighest);
    out.onPrimaryContainer = color(roles.onSurface);
    out.secondary = color(roles.secondary);
    out.onSecondary = color(roles.surface);
    out.secondaryContainer = color(roles.surfaceContainerHigh);
    out.onSecondaryContainer = color(roles.onSurface);
    out.tertiary = color(roles.tertiary);
    out.onTertiary = color(roles.surface);
    out.hover = color(roles.surfaceContainerHigh);
    out.onHover = color(roles.onSurface);
    out.shadow = rgba(0, 0, 0, 1);
    out.scrim = rgba(0, 0, 0, 1);
    return out;
  }

  // Frames until the folder is measured and the motion has ended.
  void rest(Rig& rig) {
    rig.until([&rig] {
      const auto snapshot = rig.model.snapshot();
      return snapshot && !rig.model.measuring() && !rig.animations.hasActive();
    });
  }

  std::vector<std::string> sentences(const QuietColumnView& column) {
    std::vector<const Label*> labels;
    collect(column, labels);
    std::vector<std::string> out;
    for (const auto* label : labels) {
      if (!label->text().empty()) {
        out.push_back(label->text());
      }
    }
    return out;
  }

  // The share of a node's pixels that differ from its most common color.
  double drawnShare(const GlPixels& pixels, const Node& node) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    x -= node.translateX();
    y += 4; // clear of the rule between the ribbon and the center
    std::unordered_map<std::uint32_t, std::size_t> counts;
    std::size_t total = 0;
    for (int py = std::max(0, static_cast<int>(y));
         py < std::min(pixels.height, static_cast<int>(y + node.height() - 4)); ++py) {
      for (int px = std::max(0, static_cast<int>(x) + 3);
           px < std::min(pixels.width, static_cast<int>(x + node.width()) - 3); ++px) {
        const auto p = pixels.at(px, py);
        ++counts[(std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) | (std::uint32_t{p[2]} << 8U) | p[3]];
        ++total;
      }
    }
    std::size_t most = 0;
    for (const auto& [value, count] : counts) {
      most = std::max(most, count);
    }
    return total == 0 ? 0 : static_cast<double>(total - most) / static_cast<double>(total);
  }

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir != nullptr && dir[0] != '\0') {
      TEST_CHECK(::test::writePng(pixels, std::filesystem::path(dir) / name));
    }
  }

  void noFoldersSaysSoOnce(Rig& rig, ::test::SceneCapture& capture) {
    rig.open("small");
    rest(rig);
    rig.finishTransitions();
    const auto& first = rig.view->column(0);
    TEST_CHECK(first.notice() == QuietNotice::NoFolders && !first.blank());
    const auto said = sentences(first);
    TEST_CHECK(said.size() == 1 && said.front() == "No folders ahead");
    TEST_CHECK(rig.view->center().plan().columns.size() == 3); // all three are laid out
    for (const std::size_t slot : {1UZ, 2UZ}) {
      const auto& column = rig.view->column(slot);
      TEST_CHECK(column.blank() && sentences(column).empty());
    }
    rig.view->update();
    const auto pixels = capture.capture(*rig.view, kWidth, kHeight, 1);
    keepForViewing(pixels, "workbench-columns-no-folders.png");
    const double sentence = drawnShare(pixels, rig.view->column(0));
    const double second = drawnShare(pixels, rig.view->column(1));
    const double third = drawnShare(pixels, rig.view->column(2));
    std::println("no folders: column A {:.4f} drawn, B {:.4f}, C {:.4f}", sentence, second, third);
    TEST_CHECK(sentence > 0.002 && second < 0.0005 && third < 0.0005);
  }

  void foldersListAgain(Rig& rig) {
    rig.open("big");
    rig.settle();
    const auto& first = rig.view->column(0);
    TEST_CHECK(first.notice() == QuietNotice::None && !first.blank());
    TEST_CHECK(!sentences(first).empty());
    for (const auto& text : sentences(first)) {
      TEST_CHECK(text != "No folders ahead");
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
  setPalette(darkPalette());
  Rig rig;
  rig.view->setPalette(workbenchPalette(palette));
  rig.resize(kWidth, kHeight);
  noFoldersSaysSoOnce(rig, *capture);
  foldersListAgain(rig);
  std::println("workbench columns empty checks passed");
  return 0;
}
