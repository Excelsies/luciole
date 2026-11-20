#include "png_writer.h"
#include "render/scene/node.h"
#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/palette.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/workbench_chrome.h"
#include "workbench/scene_shapes.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The three ribbons drawn with real text and icons, as a pane shows them.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;
  using ::test::GlPixels;
  using ::test::SceneCapture;

  constexpr float kWidth = 1060;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};
  constexpr NodeId kProj = 5;
  constexpr std::string_view kCoreSize = "66.8 MiB"; // the folder "core", 7e7 bytes

  Color color(Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  // The default dark palette, as the scene builders keep its roles.
  Palette darkPalette() {
    const ColorPalette roles;
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
    out.secondary = color(roles.secondary);
    out.onSecondary = color(roles.surface);
    out.tertiary = color(roles.tertiary);
    out.onTertiary = color(roles.surface);
    out.shadow = rgba(0, 0, 0, 1);
    out.scrim = rgba(0, 0, 0, 1);
    return out;
  }

  // / > home > user > projects > work > proj, entered at proj: five ancestors, each with the way down as its heaviest
  // entry. proj holds five folders, one still measuring, and three files; `filesOnly` leaves out its folders.
  Tree projectTree(bool filesOnly) {
    std::vector<WorkbenchRow> rows{
        {.parent = -1, .name = "/", .folder = true, .bytes = 9e9, .items = 3},
        {.parent = 0, .name = "home", .folder = true, .bytes = 5e9, .items = 2},
        {.parent = 1, .name = "user", .folder = true, .bytes = 4e9, .items = 3},
        {.parent = 2, .name = "projects", .folder = true, .bytes = 3e9, .items = 2},
        {.parent = 3, .name = "work", .folder = true, .bytes = 2e9, .items = 3},
        {.parent = 4, .name = "proj", .folder = true, .bytes = 1.2e9, .items = filesOnly ? 3.0 : 8.0},
        {.parent = 0, .name = "usr", .folder = true, .bytes = 3e9, .items = 1},
        {.parent = 0, .name = "var", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 1, .name = "guest", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 2, .name = "Music", .folder = true, .bytes = 6e8, .items = 1},
        {.parent = 2, .name = "Pictures", .folder = true, .bytes = 4e8, .items = 1},
        {.parent = 3, .name = "luciole", .folder = true, .bytes = 1e9, .items = 1},
        {.parent = 4, .name = "scratch", .folder = true, .bytes = 6e8, .items = 1},
        {.parent = 4, .name = "todo.txt", .bytes = 2e8},
        {.parent = 5, .name = "README.md", .bytes = 1.5e8},
        {.parent = 5, .name = "data.csv", .bytes = 1e8},
        {.parent = 5, .name = "script.py", .bytes = 5e7},
    };
    if (!filesOnly) {
      const std::vector<WorkbenchRow> folders{
          {.parent = 5, .name = "assets", .folder = true, .bytes = 3.5e8, .items = 2},
          {.parent = 5, .name = "build", .folder = true, .bytes = 2.5e8, .items = 1},
          {.parent = 5, .name = "docs", .folder = true, .bytes = 1.5e8, .items = 1},
          {.parent = 5, .name = "src", .folder = true, .bytes = 1.2e8, .items = 2},
          {.parent = 5, .name = "tests", .folder = true, .bytes = 3e7, .items = 0},
          {.parent = 17, .name = "sounds", .folder = true, .bytes = 2e8, .items = 1},
          {.parent = 17, .name = "big.bin", .bytes = 1.5e8},
          {.parent = 18, .name = "deps", .folder = true, .bytes = 2.5e8, .items = 0},
          {.parent = 19, .name = "img", .folder = true, .bytes = 1.5e8, .items = 0},
          {.parent = 20, .name = "core", .folder = true, .bytes = 7e7, .items = 0},
          {.parent = 20, .name = "ui", .folder = true, .bytes = 5e7, .items = 0},
          {.parent = 22, .name = "loops", .folder = true, .bytes = 2e8, .items = 0},
      };
      rows.insert(rows.end(), folders.begin(), folders.end());
    }
    auto tree = luciole::test::workbenchTree(rows, 1000);
    tree.root = kProj;
    tree.ancestors = parentChain(tree, kProj);
    if (!filesOnly) {
      tree.nodes[18].partial = true; // build
    }
    return tree;
  }

  struct Pane {
    ::Node root;
    RibbonView* view = nullptr;

    explicit Pane(WorkbenchRibbon kind) {
      auto ribbon = std::make_unique<RibbonView>(kind, [] { return 0U; });
      view = ribbon.get();
      root.addChild(paneBody(std::move(ribbon)));
    }

    // Lays the pane out until the part shows `tree`, then captures it.
    GlPixels show(SceneCapture& capture, const Tree& tree, float height, float scale, float width = kWidth) {
      const PartInputs inputs{
          .snapshot = {.tree = std::make_shared<const Tree>(tree), .serial = 1},
          .settings = {},
          .palette = workbenchPalette(palette),
      };
      root.children().front()->setSize(width, height);
      for (int round = 0; round < 3; ++round) {
        (void)capture.capture(root, static_cast<int>(width), static_cast<int>(height), scale);
        view->update(inputs, false);
      }
      return capture.capture(root, static_cast<int>(width), static_cast<int>(height), scale);
    }

    const Shape* shape(NodeId node, SceneRole role, bool hittable) const {
      for (const auto& each : view->shapeView().shapes()) {
        if (each.id.node == node && sceneRole(each.id.role) == role && each.hittable == hittable) {
          return &each;
        }
      }
      return nullptr;
    }
  };

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // Whether the device pixel under logical (x, y) differs from `background` by more than `tolerance` in a channel.
  bool drawnAt(const GlPixels& pixels, float x, float y, float scale, Rgb background, int tolerance = 12) {
    const int px = std::clamp(static_cast<int>(std::floor(x * scale)), 0, pixels.width - 1);
    const int py = std::clamp(static_cast<int>(std::floor(y * scale)), 0, pixels.height - 1);
    const auto p = pixels.at(px, py);
    return std::abs(p[0] - background.r) > tolerance
        || std::abs(p[1] - background.g) > tolerance
        || std::abs(p[2] - background.b) > tolerance;
  }

  // Device pixels in a logical box that differ from `background`.
  int drawnIn(const GlPixels& pixels, ShapeRect box, float scale, Rgb background) {
    int count = 0;
    for (int y = static_cast<int>(std::ceil(box.y * scale)); y < static_cast<int>((box.y + box.height) * scale); ++y) {
      for (int x = static_cast<int>(std::ceil(box.x * scale)); x < static_cast<int>((box.x + box.width) * scale); ++x) {
        count += drawnAt(
                     pixels, (static_cast<float>(x) + 0.5F) / scale, (static_cast<float>(y) + 0.5F) / scale, scale,
                     background
                 )
            ? 1
            : 0;
      }
    }
    return count;
  }

  // Middle of the device columns in a logical box that hold anything but `background`.
  std::optional<float> inkMiddle(const GlPixels& pixels, ShapeRect box, float scale, Rgb background) {
    std::optional<int> first;
    int end = 0;
    for (int x = static_cast<int>(std::ceil(box.x * scale)); x < static_cast<int>((box.x + box.width) * scale); ++x) {
      for (int y = static_cast<int>(std::ceil(box.y * scale)); y < static_cast<int>((box.y + box.height) * scale);
           ++y) {
        if (drawnAt(
                pixels, (static_cast<float>(x) + 0.5F) / scale, (static_cast<float>(y) + 0.5F) / scale, scale,
                background, 40
            )) {
          first = first.value_or(x);
          end = x + 1;
          break;
        }
      }
    }
    if (!first) {
      return std::nullopt;
    }
    return static_cast<float>(*first + end) / 2 / scale;
  }

  // The texts shown that equal `size`. Checks that no shown size is cut.
  int shownSizes(const Pane& pane, std::string_view size) {
    int count = 0;
    for (const auto& child : pane.view->shapeView().labelLayer().children()) {
      const auto* text = dynamic_cast<const TextNode*>(child.get());
      if (text == nullptr || !text->visible()) {
        continue;
      }
      TEST_CHECK(!text->text().ends_with("iB") || !text->text().contains("…"));
      count += text->text() == size ? 1 : 0;
    }
    return count;
  }

  // The current folder's bar shows its icon before its name; a folder of files has a dashed outline over the strata
  // ahead with a centered caption.
  void flame(SceneCapture& capture) {
    const ColorPalette roles;
    for (const float scale : kScales) {
      Pane pane(WorkbenchRibbon::Flame);
      auto pixels = pane.show(capture, projectTree(false), 224, scale);
      keepForViewing(pixels, std::format("ribbon-flame-{}x.png", scale));
      // "core" in its narrow cell shows its whole size, under its name here and beside it in a lower ribbon.
      TEST_CHECK(shownSizes(pane, kCoreSize) == 1);
      Pane low(WorkbenchRibbon::Flame);
      keepForViewing(low.show(capture, projectTree(false), 160, scale), std::format("ribbon-flame-low-{}x.png", scale));
      TEST_CHECK(shownSizes(low, kCoreSize) == 1);
      const auto* bar = pane.shape(kProj, SceneRole::Path, true);
      TEST_CHECK(bar != nullptr);
      if (bar != nullptr) {
        const float middle = bar->rect.y + bar->rect.height / 2;
        const ShapeRect icon{.x = bar->rect.x + 10, .y = middle - 8, .width = 16, .height = 16, .radius = 0};
        TEST_CHECK(drawnIn(pixels, icon, scale, roles.surfaceContainerHigh) > static_cast<int>(20 * scale * scale));
      }

      // Every folder still measuring: each piece hatched.
      auto measuring = projectTree(false);
      for (auto& node : measuring.nodes) {
        node.partial = node.folder;
      }
      Pane hatched(WorkbenchRibbon::Flame);
      pixels = hatched.show(capture, measuring, 224, scale);
      keepForViewing(pixels, std::format("ribbon-flame-measuring-{}x.png", scale));

      Pane empty(WorkbenchRibbon::Flame);
      pixels = empty.show(capture, projectTree(true), 224, scale);
      keepForViewing(pixels, std::format("ribbon-flame-files-{}x.png", scale));
      const auto* outline = empty.shape(kProj, SceneRole::Detail, false);
      TEST_CHECK(outline != nullptr && outline->pattern == ShapePattern::DashedOutline);
      if (outline == nullptr) {
        continue;
      }
      const auto& box = outline->rect;
      // Dashes: along the top edge some pixels carry the outline and some do not.
      int on = 0;
      int samples = 0;
      for (float x = box.x + 12; x < box.x + box.width - 12; x += 1 / scale) {
        on += drawnAt(pixels, x, box.y + 0.5F / scale, scale, roles.surfaceContainerLow, 6) ? 1 : 0;
        ++samples;
      }
      std::println("{}x flame dashes: {} of {} pixels drawn", scale, on, samples);
      TEST_CHECK(on > samples / 4 && on < samples * 3 / 4);
      const ShapeRect inside{
          .x = box.x + 4, .y = box.y + 4, .width = box.width - 8, .height = box.height - 8, .radius = 0
      };
      const auto middle = inkMiddle(pixels, inside, scale, roles.surfaceContainerLow);
      TEST_CHECK(middle.has_value());
      if (middle) {
        std::println("{}x flame caption middle {:.2f}, box middle {:.2f}", scale, *middle, box.x + box.width / 2);
        TEST_NEAR(*middle, box.x + box.width / 2, 1.0F / scale);
      }
    }
  }

  // One bar per ancestor at the default height, and the bar's outlined first piece rounded at the left end like the
  // bar's right end.
  void ladder(SceneCapture& capture) {
    const ColorPalette roles;
    for (const float scale : kScales) {
      Pane pane(WorkbenchRibbon::Ladder);
      const auto tree = projectTree(false);
      (void)pane.show(capture, tree, 200, scale);
      const auto height = static_cast<float>(pane.view->limits().preferredHeight);
      const auto pixels = pane.show(capture, tree, height, scale);
      keepForViewing(pixels, std::format("ribbon-ladder-{}x.png", scale));
      std::println("{}x ladder at its preferred height {}", scale, height);
      // Bars down a column near the right end, where no outline joins them.
      int bars = 0;
      bool inBar = false;
      for (int y = 0; y < pixels.height; ++y) {
        const bool drawn =
            drawnAt(pixels, kWidth - 16, (static_cast<float>(y) + 0.5F) / scale, scale, roles.surfaceContainerLow, 6);
        bars += drawn && !inBar ? 1 : 0;
        inBar = drawn;
      }
      TEST_CHECK(bars == static_cast<int>(tree.ancestors.size()));
      const auto* first = pane.shape(kProj, SceneRole::Piece, true);
      TEST_CHECK(first != nullptr);
      if (first == nullptr) {
        continue;
      }
      const auto& r = first->rect;
      // The outline's outer corner is cut round; its left side is drawn.
      TEST_CHECK(
          !drawnAt(pixels, r.x - 2 + 0.3F / scale, r.y - 2 + 0.3F / scale, scale, roles.surfaceContainerLow, 30)
      );
      TEST_CHECK(drawnAt(pixels, r.x - 1, r.y + r.height / 2, scale, roles.surfaceContainerLow));
    }
  }

  // Labels of the last column fill the room kept for them at the right; a folder of files shows the note with a
  // folder icon over its title.
  void flow(SceneCapture& capture) {
    const ColorPalette roles;
    for (const float scale : kScales) {
      Pane pane(WorkbenchRibbon::Flow);
      auto pixels = pane.show(capture, projectTree(false), 280, scale);
      keepForViewing(pixels, std::format("ribbon-flow-{}x.png", scale));
      const ShapeRect right{.x = kWidth * 0.83F, .y = 0, .width = kWidth * 0.17F - 12, .height = 280, .radius = 0};
      TEST_CHECK(drawnIn(pixels, right, scale, roles.surfaceContainerLow) > static_cast<int>(100 * scale * scale));

      Pane empty(WorkbenchRibbon::Flow);
      pixels = empty.show(capture, projectTree(true), 280, scale);
      keepForViewing(pixels, std::format("ribbon-flow-files-{}x.png", scale));
      const auto& shapes = empty.view->shapeView().shapes();
      const auto bar = std::ranges::find_if(shapes, [](const Shape& s) {
        return s.id.node == kProj && s.hittable && sceneRole(s.id.role) == SceneRole::Piece;
      });
      TEST_CHECK(bar != shapes.end());
      if (bar == shapes.end()) {
        continue;
      }
      // The icon box: 24 px centered 40 px right of the middle of the room right of the bar, its top 38 px above
      // the bar's middle.
      const float cx = bar->rect.x + (kWidth - bar->rect.x) / 2 + 40;
      const float cy = bar->rect.y + bar->rect.height / 2;
      const ShapeRect icon{.x = cx - 12, .y = cy - 38, .width = 24, .height = 24, .radius = 0};
      TEST_CHECK(drawnIn(pixels, icon, scale, roles.surfaceContainerLow) > static_cast<int>(30 * scale * scale));
    }
  }

  // An ancestor pill is drawn only with its name: in a narrow ribbon every pill tall enough for a name holds ink
  // other than its own fill, and a pill whose name does not fit is left out whole.
  void flowPillsAreNeverBlank(SceneCapture& capture) {
    for (const float scale : kScales) {
      for (const float width : {560.0F, 640.0F, 800.0F}) {
        Pane pane(WorkbenchRibbon::Flow);
        const auto pixels = pane.show(capture, projectTree(false), 280, scale, width);
        keepForViewing(pixels, std::format("ribbon-flow-pills-{}-{}x.png", width, scale));
        int named = 0;
        for (const auto& pill : pane.view->shapeView().shapes()) {
          if (sceneRole(pill.id.role) != SceneRole::Stem || pill.kind != ShapeKind::RoundedRect) {
            continue;
          }
          if (pill.rect.height < 20) {
            continue;
          }
          ++named;
          const auto& r = pill.rect;
          int ink = 0;
          for (int y = static_cast<int>(std::ceil((r.y + 3) * scale));
               y < static_cast<int>((r.y + r.height - 3) * scale); ++y) {
            for (int x = static_cast<int>(std::ceil((r.x + 4) * scale));
                 x < static_cast<int>((r.x + r.width - 4) * scale); ++x) {
              const auto p = pixels.at(x, y);
              ink += std::abs(p[0] - pill.fill.r) + std::abs(p[1] - pill.fill.g) + std::abs(p[2] - pill.fill.b) > 90
                  ? 1
                  : 0;
            }
          }
          std::println("pill {:.0f}x{:.0f} in {} px at {}x: {} px of name", r.width, r.height, width, scale, ink);
          TEST_CHECK(ink > 8);
        }
        TEST_CHECK(named >= 1);
      }
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  setPalette(darkPalette());
  flame(*capture);
  ladder(*capture);
  flow(*capture);
  flowPillsAreNeverBlank(*capture);
  std::println("ribbon pixel checks passed");
  return 0;
}
