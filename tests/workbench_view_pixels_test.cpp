#include "png_writer.h"
#include "scene_capture.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/pane_header.h"
#include "ui/palette.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/iris_hit.h"
#include "workbench_view_rig.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::view;
  namespace wb = luciole::workbench;
  using ::test::GlPixels;

  constexpr int kWidth = 1400;
  constexpr int kHeight = 900;
  constexpr std::array<float, 2> kScales{1.0F, 1.5F};

  Color color(wb::Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  // The default dark palette, as the scene builders keep its roles.
  Palette darkPalette() {
    const wb::ColorPalette roles;
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

  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  // Device pixels inside a node's laid-out bounds.
  Region regionOf(const Node& node, float scale) {
    float x = 0;
    float y = 0;
    Node::absolutePosition(&node, x, y);
    return {
        .left = static_cast<int>(std::ceil(x * scale)),
        .top = static_cast<int>(std::ceil(y * scale)),
        .right = static_cast<int>(std::floor((x + node.width()) * scale)),
        .bottom = static_cast<int>(std::floor((y + node.height()) * scale)),
    };
  }

  // The share of a region's pixels that differ from its most common color: what was drawn over the background.
  double drawnShare(const GlPixels& pixels, Region region) {
    std::unordered_map<std::uint32_t, std::size_t> counts;
    std::size_t total = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, pixels.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, pixels.width); ++x) {
        const auto p = pixels.at(x, y);
        ++counts[(std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U) | (std::uint32_t{p[2]} << 8U) | p[3]];
        ++total;
      }
    }
    std::size_t most = 0;
    for (const auto& [rgba, count] : counts) {
      most = std::max(most, count);
    }
    return total == 0 ? 0 : static_cast<double>(total - most) / static_cast<double>(total);
  }

  void expectDrawn(const GlPixels& pixels, const Node& node, float scale, const char* what, double least) {
    const double share = drawnShare(pixels, regionOf(node, scale));
    std::println("{}x {}: {:.3f} drawn", scale, what, share);
    TEST_CHECK(share >= least);
  }

  // Frames until `ready` holds, else until the view is loaded and at rest, drawing each one; returns the last capture.
  GlPixels settle(Rig& rig, ::test::SceneCapture& capture, float scale, const std::function<bool()>& ready = {}) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    GlPixels pixels;
    for (bool done = false; !done;) {
      TEST_CHECK(Clock::now() < deadline);
      rig.pump.once();
      rig.animations.tick(16);
      rig.view->update();
      pixels = capture.capture(*rig.view, kWidth, kHeight, scale);
      done = ready ? ready() : rig.loaded();
    }
    rig.view->update();
    return capture.capture(*rig.view, kWidth, kHeight, scale);
  }

  void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // The inside of a column's slot at 1x, where it rests, clear of the dividers on its edges.
  Region slotOf(const Node& column) {
    constexpr int kInset = 3;
    auto region = regionOf(column, 1);
    const auto shift = static_cast<int>(std::lround(column.translateX()));
    return {
        .left = region.left - shift + kInset,
        .top = region.top,
        .right = region.right - shift - kInset,
        .bottom = region.bottom
    };
  }

  // Entering a folder starts with the columns ahead faded out and the strips half faded, away from their places.
  void motionStartsFaded(Rig& rig, ::test::SceneCapture& capture) {
    const auto rest = capture.capture(*rig.view, kWidth, kHeight, 1);
    const double before = drawnShare(rest, slotOf(rig.view->column(0)));
    rig.open("big");
    rig.view->update();
    TEST_CHECK(rig.view->center().moving());
    const auto start = capture.capture(*rig.view, kWidth, kHeight, 1);
    keepForViewing(start, "workbench-view-motion-start.png");
    const double after = drawnShare(start, slotOf(rig.view->column(0)));
    std::println("column A: {:.3f} drawn at rest, {:.3f} as the motion starts", before, after);
    TEST_CHECK(before >= 0.02 && after < 0.005);
    TEST_NEAR(rig.view->center().strips().opacity(), 0.5F, 0.001F);
  }

  // A file previewed in column A in the frame that enters a folder leaves column B drawn once the preview goes.
  void columnsShowAfterAPreviewDuringTheMotion(Rig& rig, ::test::SceneCapture& capture) {
    (void)settle(rig, capture, 1);
    (void)rig.view->setPreview(std::make_unique<::Node>()); // the page's preview slot
    rig.open("");
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("note.txt"), .folder = false});
    rig.view->update();
    TEST_CHECK(rig.view->center().moving() && rig.lookahead.preview().has_value());
    (void)settle(rig, capture, 1, [&rig] { return !rig.animations.hasActive(); });
    rig.view->setCursor(WorkbenchEntry{.location = rig.fixture.at("big"), .folder = true});
    const auto pixels = settle(rig, capture, 1, [&rig] {
      return rig.loaded() && rig.lookahead.count() > 1 && !rig.lookahead.preview();
    });
    keepForViewing(pixels, "workbench-view-columns-after-preview.png");
    const double share = drawnShare(pixels, slotOf(rig.view->column(1)));
    std::println("column B after a preview: {:.3f} drawn", share);
    TEST_CHECK(share >= 0.02);
  }

  // The device pixels of the expanded instrument's trail chips, in order.
  std::vector<Region> chipRegions(const Rig& rig) {
    const auto& part = rig.view->instrument().shapeView();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&part, x, y);
    std::vector<Region> regions;
    for (std::size_t index = 0; index < 3; ++index) {
      for (const auto& shape : part.shapes()) {
        if (shape.id == wb::iris::chipId(index) && shape.hittable) {
          const auto& rect = shape.rect;
          regions.push_back({
              .left = static_cast<int>(std::ceil(x + rect.x)),
              .top = static_cast<int>(std::ceil(y + rect.y)),
              .right = static_cast<int>(std::floor(x + rect.x + rect.width)),
              .bottom = static_cast<int>(std::floor(y + rect.y + rect.height)),
          });
        }
      }
    }
    return regions;
  }

  bool samePixels(const GlPixels& left, const GlPixels& right, Region region) {
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        if (left.at(x, y) != right.at(x, y)) {
          return false;
        }
      }
    }
    return true;
  }

  // After big/deep and back up, the expanded Iris hub offers the page's trail as two chips, drawn the same after a
  // switch to Nest and back.
  void irisChipsShowThePageTrail(Rig& rig, ::test::SceneCapture& capture) {
    rig.view->setCursor(std::nullopt);
    rig.open("big/deep");
    (void)settle(rig, capture, 1);
    rig.open("");
    (void)settle(rig, capture, 1);
    auto geometry = rig.view->geometry();
    geometry.panes.expanded = true;
    rig.view->setGeometry(geometry);
    const auto choose = [&rig](WorkbenchInstrument kind) {
      auto choices = rig.view->choices();
      choices.instrument = kind;
      rig.view->setChoices(choices);
    };
    const auto chipsShown = [&rig] { return !rig.animations.hasActive() && chipRegions(rig).size() == 2; };
    choose(WorkbenchInstrument::Iris);
    const auto first = settle(rig, capture, 1, chipsShown);
    keepForViewing(first, "workbench-view-iris-trail.png");
    const auto chips = chipRegions(rig);
    TEST_CHECK(chips.size() == 2);
    for (const auto& chip : chips) {
      const double share = drawnShare(first, chip);
      std::println("trail chip: {:.3f} drawn", share);
      TEST_CHECK(share >= 0.05);
    }
    choose(WorkbenchInstrument::Nest);
    (void)settle(rig, capture, 1, [&rig] { return !rig.animations.hasActive() && chipRegions(rig).empty(); });
    choose(WorkbenchInstrument::Iris);
    const auto again = settle(rig, capture, 1, chipsShown);
    keepForViewing(again, "workbench-view-iris-trail-after-switch.png");
    const auto after = chipRegions(rig);
    TEST_CHECK(after.size() == chips.size());
    for (std::size_t i = 0; i < after.size() && i < chips.size(); ++i) {
      TEST_CHECK(after[i].left == chips[i].left && after[i].top == chips[i].top);
      TEST_CHECK(samePixels(first, again, chips[i]));
    }
  }

  // The strips pane at its narrowest, its default and a wide width, in folders 3, 5 and 10 levels deep: the bands sit
  // against the list, the room beyond the farthest stays empty, and the list starts where the pane ends.
  void stripsPaneWidths(Rig& rig, ::test::SceneCapture& capture) {
    const fs::path chain = "c1/c2/c3/c4/c5/c6/c7/c8/c9/c10"; // enough for 10 ancestors from any temporary folder
    Fixture::write(rig.fixture.root / chain / "leaf" / "x.txt", 300);
    rig.open("");
    auto geometry = rig.view->geometry();
    geometry.panes.expanded = false;
    rig.view->setGeometry(geometry);
    (void)settle(rig, capture, 1);
    const auto& center = rig.view->center();
    const std::size_t base = center.ancestors().size(); // the fixture root's ancestors, from the temporary folder
    for (const std::size_t depth : {3U, 5U, 10U}) {
      if (depth < base) {
        std::println("no capture with {} levels: the fixture root already has {} ancestors", depth, base);
        continue;
      }
      fs::path folder;
      auto part = chain.begin();
      for (std::size_t level = base; level < depth && part != chain.end(); ++level, ++part) { // at most 10 parts
        folder /= *part;
      }
      rig.open(folder);
      for (const double width : {70.0, 153.0, 260.0}) {
        geometry.strips = width;
        rig.view->setGeometry(geometry);
        const auto pixels = settle(rig, capture, 1);
        keepForViewing(pixels, std::format("workbench-strips-{}-levels-{}px.png", depth, width));
        TEST_CHECK(center.ancestors().size() == depth && center.plan().stripsWidth == width);
        const auto pane = regionOf(center.strips(), 1);
        TEST_CHECK(regionOf(*center.list(), 1).left == pane.right);
        expectDrawn(pixels, center.strips(), 1, std::format("{} levels in {} px", depth, width).c_str(), 0.05);
        // the room beyond the farthest band, clear of the shell's divider lines on the pane's far and top edges
        const int farthest = regionOf(*center.strips().area(0), 1).left;
        const int inset = static_cast<int>(PaneDivider::kHitSize / 2);
        const int roomLeft = pane.left + inset;
        if (farthest - roomLeft >= 2) {
          const double empty = drawnShare(
              pixels, {.left = roomLeft, .top = pane.top + inset, .right = farthest - 1, .bottom = pane.bottom}
          );
          std::println(
              "{} levels in {} px: {} px beyond the bands, {:.4f} drawn", depth, width, farthest - pane.left, empty
          );
          TEST_CHECK(empty == 0);
        }
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
  setPalette(darkPalette());
  Rig rig;
  rig.view->setPalette(workbenchPalette(palette));
  WorkbenchGeometry narrowStrips; // as wide as the fixture's three bands, so a second column ahead fits
  narrowStrips.strips = 107;
  rig.view->setGeometry(narrowStrips);
  for (const float scale : kScales) {
    const auto pixels = settle(rig, *capture, scale);
    TEST_CHECK(pixels.width == static_cast<int>(std::lround(kWidth * scale)));
    keepForViewing(pixels, std::format("workbench-view-{}x.png", scale));
    const auto& shell = rig.view->shell();
    expectDrawn(pixels, rig.view->instrument(), scale, "instrument", 0.2);
    expectDrawn(pixels, rig.view->ribbon(), scale, "ribbon", 0.1);
    expectDrawn(pixels, rig.view->jumpList(), scale, "jump ahead", 0.01);
    expectDrawn(pixels, rig.jumpRow("big/deep")->valueLabel(), scale, "jump ahead value", 0.05);
    expectDrawn(pixels, rig.view->column(0), scale, "column A", 0.02);
    expectDrawn(pixels, rig.view->column(1), scale, "column B", 0.02);
    expectDrawn(pixels, rig.view->center().strips(), scale, "strips", 0.05);
    for (const auto pane : {wb::Pane::Instrument, wb::Pane::Jump, wb::Pane::Ribbon}) {
      const auto* title = shell.header(pane).titleLabel();
      expectDrawn(pixels, *title, scale, title->text().c_str(), 0.02);
    }
  }
  motionStartsFaded(rig, *capture);
  columnsShowAfterAPreviewDuringTheMotion(rig, *capture);
  irisChipsShowThePageTrail(rig, *capture);
  stripsPaneWidths(rig, *capture);
  std::println("workbench-view-pixels: all checks passed");
  return 0;
}
