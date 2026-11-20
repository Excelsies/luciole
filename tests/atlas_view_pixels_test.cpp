#include "png_writer.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/color.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench_fixture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Atlas instrument drawn offscreen with real text: the bar of a small pane, the chips and buttons of a large one,
// labels on title chips and pills, a pane that grew, a pan, a flight and a cross-fade.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using ::test::GlPixels;
  using ::test::SceneCapture;
  using Clock = std::chrono::steady_clock;

  // / > home > user > proj, where proj holds folders of several sizes, one with more entries than are shown, and
  // files, one with a long name.
  Tree projectTree() {
    std::vector<luciole::test::WorkbenchRow> rows;
    std::vector<std::string> names;
    names.reserve(400);
    const auto add = [&](int parent, std::string name, bool folder, double bytes) {
      names.push_back(std::move(name));
      rows.push_back({.parent = parent, .name = names.back(), .folder = folder, .bytes = bytes});
      return static_cast<int>(rows.size()) - 1;
    };
    const int top = add(-1, "", true, 0);
    const int home = add(top, "home", true, 0);
    const int user = add(home, "user", true, 0);
    const int proj = add(user, "proj", true, 0);
    const auto files = [&](int folder, const char* stem, int count, double bytes) {
      for (int i = 0; i < count; ++i) {
        add(folder, std::format("{}{:02}.txt", stem, i), false, bytes * (1.0 + (i % 5) * 0.4));
      }
    };
    const int src = add(proj, "src", true, 0);
    files(src, "main", 10, 4000);
    files(add(src, "core", true, 0), "core", 6, 3000);
    files(add(src, "ui", true, 0), "view", 5, 2500);
    const int docs = add(proj, "docs", true, 0);
    files(docs, "guide", 6, 2000);
    files(add(docs, "img", true, 0), "shot", 4, 9000);
    const int assets = add(proj, "assets", true, 0);
    files(add(assets, "sounds", true, 0), "beep", 3, 8000);
    files(assets, "icon", 5, 3000);
    files(add(proj, "build", true, 0), "obj", 8, 5000);
    files(add(proj, "tests", true, 0), "test", 10, 1500);
    files(add(proj, "logs", true, 0), "day", 75, 400);
    add(proj, "README.md", false, 3000);
    add(proj, "notes.txt", false, 1200);
    add(proj, "a-very-long-file-name-for-two-lines.txt", false, 14000);
    add(proj, "data.csv", false, 6000);
    auto tree = luciole::test::workbenchTree(rows, 1e6);
    // Folder sizes are the sums below them, as the measure gives them.
    for (std::size_t i = tree.nodes.size(); i-- > 1;) {
      tree.nodes[tree.nodes[i].parent].bytes += tree.nodes[i].bytes;
    }
    tree.root = static_cast<NodeId>(proj);
    tree.ancestors = parentChain(tree, tree.root);
    return tree;
  }

  PartInputs inputsFor(Tree tree, std::uint64_t serial) {
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = serial},
        .settings = {},
        .palette = {},
    };
  }

  // An instrument view at a size, framed and drawn by the capture rig.
  struct Pane {
    AnimationManager animations;
    ::Node root;
    InstrumentView* view = nullptr;
    InputDispatcher dispatcher;
    std::size_t frameRequests = 0;
    int width = 0;
    int height = 0;

    Pane(int w, int h) : width(w), height(h) {
      root.setSize(4000, 4000);
      root.setAnimationManager(&animations);
      view = static_cast<InstrumentView*>(
          root.addChild(std::make_unique<InstrumentView>(WorkbenchInstrument::Atlas, [] { return 0U; }))
      );
      view->onFrameNeeded = [this] { ++frameRequests; };
      dispatcher.setSceneRoot(&root);
    }

    GlPixels frame(SceneCapture& capture, const PartInputs& inputs, float scale) {
      while (g_main_context_iteration(nullptr, FALSE) != 0) {
      }
      view->update(inputs, false);
      return capture.capture(*view, width, height, scale);
    }

    // Frames until thirty in a row ask for no frame and change no shape, at most ten seconds.
    GlPixels settle(SceneCapture& capture, const PartInputs& inputs, float scale) {
      const auto deadline = Clock::now() + std::chrono::seconds(10);
      std::vector<Shape> last;
      std::size_t asked = frameRequests;
      GlPixels pixels;
      for (int quiet = 0; quiet < 30;) {
        TEST_CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pixels = frame(capture, inputs, scale);
        const auto shapes = view->shapeView().shapes();
        const bool same = frameRequests == asked && std::ranges::equal(shapes, last) && !shapes.empty();
        quiet = same ? quiet + 1 : 0;
        asked = frameRequests;
        last.assign(shapes.begin(), shapes.end());
      }
      return pixels;
    }
  };

  // The capture over the palette's surface: the rig's pixels are premultiplied with nothing under them.
  GlPixels flat(const GlPixels& pixels) {
    const Rgb surface = ColorPalette{}.surface;
    const std::array<int, 3> under{surface.r, surface.g, surface.b};
    GlPixels out = pixels;
    for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
      const int alpha = pixels.rgba[i + 3];
      for (std::size_t c = 0; c < 3; ++c) {
        const int value = pixels.rgba[i + c] + (under[c] * (255 - alpha) + 127) / 255;
        out.rgba[i + c] = static_cast<std::uint8_t>(std::min(value, 255));
      }
      out.rgba[i + 3] = 255;
    }
    return out;
  }

  void keep(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / (name + ".png");
    TEST_CHECK(::test::writePng(flat(pixels), path));
    std::println("wrote {}", path.string());
  }

  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  Region scaled(double x, double y, double w, double h, float scale) {
    return {
        .left = static_cast<int>(std::ceil(x * scale)),
        .top = static_cast<int>(std::ceil(y * scale)),
        .right = static_cast<int>(std::floor((x + w) * scale)),
        .bottom = static_cast<int>(std::floor((y + h) * scale)),
    };
  }

  // The share of a region's flattened pixels that differ from the surface by more than a little.
  double drawnShare(const GlPixels& pixels, Region region) {
    const auto image = flat(pixels);
    const Rgb surface = ColorPalette{}.surface;
    std::size_t drawn = 0;
    std::size_t total = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, image.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, image.width); ++x) {
        const auto p = image.at(x, y);
        const int difference = std::abs(p[0] - surface.r) + std::abs(p[1] - surface.g) + std::abs(p[2] - surface.b);
        drawn += difference > 12 ? 1 : 0;
        ++total;
      }
    }
    return total == 0 ? 0 : static_cast<double>(drawn) / static_cast<double>(total);
  }

  // Pixels whose channels sum above `least`, as text on a chip or a pill draws them.
  std::size_t brightPixels(const GlPixels& pixels, Region region, int least = 3 * 170) {
    const auto image = flat(pixels);
    std::size_t count = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, image.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, image.width); ++x) {
        const auto p = image.at(x, y);
        count += p[0] + p[1] + p[2] > least ? 1 : 0;
      }
    }
    return count;
  }

  // The share of pixels that differ by more than a little between two captures of one size.
  double differingShare(const GlPixels& left, const GlPixels& right) {
    TEST_CHECK(left.width == right.width && left.height == right.height);
    std::size_t differ = 0;
    for (std::size_t i = 0; i + 3 < left.rgba.size() && i + 3 < right.rgba.size(); i += 4) {
      int difference = 0;
      for (std::size_t c = 0; c < 4; ++c) {
        difference = std::max(difference, std::abs(left.rgba[i + c] - right.rgba[i + c]));
      }
      differ += difference > 24 ? 1 : 0;
    }
    return static_cast<double>(differ) / static_cast<double>(left.rgba.size() / 4);
  }

  const Shape* findShape(const ShapeView& view, NodeId node, SceneRole role, ShapeKind kind) {
    for (const auto& shape : view.shapes()) {
      if (shape.id.node == node && sceneRole(shape.id.role) == role && shape.kind == kind) {
        return &shape;
      }
    }
    return nullptr;
  }

  // Every pill or chip of the map has text drawn on it: a shown text node starts inside it, its baseline within it.
  void pillsUnderTheirLabels(const ShapeView& view) {
    std::size_t checked = 0;
    for (const auto& shape : view.shapes()) {
      if (sceneRole(shape.id.role) != SceneRole::Detail
          || shape.kind != ShapeKind::RoundedRect
          || shape.id.node == kNoNode) {
        continue;
      }
      const auto& box = shape.rect;
      const bool written = std::ranges::any_of(view.labelLayer().children(), [&box](const auto& child) {
        return child->visible()
            && child->x() >= box.x - 4
            && child->x() <= box.x + box.width
            && child->y() >= box.y
            && child->y() <= box.y + box.height + 4;
      });
      TEST_CHECK(written);
      ++checked;
    }
    TEST_CHECK(checked > 0);
  }

  // The bar between the last chip and the first button, where nothing is drawn.
  Region barGap(const ShapeView& view, float scale) {
    double left = 0;
    double right = view.width();
    for (const auto& shape : view.shapes()) {
      const auto role = sceneRole(shape.id.role);
      if (role == SceneRole::Chip && shape.id.node != kNoNode) {
        left = std::max(left, static_cast<double>(shape.rect.x + shape.rect.width));
      } else if (role == SceneRole::Button) {
        right = std::min(right, static_cast<double>(shape.rect.x));
      }
    }
    TEST_CHECK(right - left > 20);
    return scaled(left + 2, 0, right - left - 4, 26, scale);
  }

  // A small pane: chips and buttons in the bar over the map, the map below it and never in the bar, at rest and zoomed
  // in.
  void smallPane(SceneCapture& capture, const PartInputs& inputs) {
    for (const float scale : {1.0F, 1.5F}) {
      Pane pane(340, 534);
      const auto pixels = pane.settle(capture, inputs, scale);
      keep(pixels, std::format("atlas-pane-340x534-{}x", scale));
      const double bar = drawnShare(pixels, scaled(0, 0, 340, 26, scale));
      const double map = drawnShare(pixels, scaled(0, 26, 340, 508, scale));
      const double gap = drawnShare(pixels, barGap(pane.view->shapeView(), scale));
      std::println("{}x pane: bar {:.3f} drawn, its gap {:.3f}, map {:.3f} drawn", scale, bar, gap, map);
      TEST_CHECK(bar > 0.08 && map > 0.3 && gap == 0);
      // The chips' names are written in the bar, in the quieter text color.
      TEST_CHECK(brightPixels(pixels, scaled(0, 0, 250, 26, scale), 3 * 140) > 20);
    }
    Pane pane(340, 534);
    pane.settle(capture, inputs, 1);
    pane.dispatcher.setFocus(&pane.view->shapeView().inputArea());
    for (int press = 0; press < 3; ++press) {
      pane.dispatcher.keyEvent(XKB_KEY_plus, '+', 0, true);
      pane.dispatcher.keyEvent(XKB_KEY_plus, '+', 0, false);
    }
    const auto zoomed = pane.settle(capture, inputs, 1);
    keep(zoomed, "atlas-pane-340x534-zoomed-1x");
    const double gap = drawnShare(zoomed, barGap(pane.view->shapeView(), 1));
    const double map = drawnShare(zoomed, scaled(0, 26, 340, 508, 1));
    std::println("zoomed pane: bar gap {:.3f} drawn, map {:.3f} drawn", gap, map);
    TEST_CHECK(gap == 0 && map > 0.3);
  }

  // A large pane: chips at the top left, buttons at the bottom right, the folder's title chip and named pills.
  void largePane(SceneCapture& capture, const PartInputs& inputs) {
    Pane pane(900, 600);
    const auto pixels = pane.settle(capture, inputs, 1);
    keep(pixels, "atlas-900x600-1x");
    const auto& view = pane.view->shapeView();
    const auto* title = findShape(view, inputs.snapshot.tree->root, SceneRole::Detail, ShapeKind::RoundedRect);
    TEST_CHECK(title != nullptr);
    const auto& chip = title->rect;
    const std::size_t written = brightPixels(pixels, scaled(chip.x, chip.y, chip.width, chip.height, 1));
    std::println("title chip {}x{} with {} bright pixels", chip.width, chip.height, written);
    TEST_CHECK(written > 20);
    // Each button draws its icon.
    for (int k = 0; k < 3; ++k) {
      const double top = 600 - 12 - 98 + 34.0 * k;
      TEST_CHECK(brightPixels(pixels, scaled(900 - 12 - 30, top, 30, 30, 1), 3 * 120) > 6);
    }
    std::size_t pills = 0;
    for (const auto& shape : view.shapes()) {
      if (sceneRole(shape.id.role) == SceneRole::Detail
          && shape.kind == ShapeKind::RoundedRect
          && shape.id.node != inputs.snapshot.tree->root) {
        const auto& box = shape.rect;
        pills += brightPixels(pixels, scaled(box.x, box.y, box.width, box.height, 1)) > 10 ? 1 : 0;
      }
    }
    std::println("{} written pills or chips over children", pills);
    TEST_CHECK(pills >= 1);
  }

  // A pane that grew draws as a fresh one of that size.
  void grownPane(SceneCapture& capture, const PartInputs& inputs) {
    Pane pane(340, 534);
    pane.settle(capture, inputs, 1);
    pane.width = 520;
    pane.height = 844;
    const auto grown = pane.settle(capture, inputs, 1);
    keep(grown, "atlas-grown-520x844-1x");
    Pane fresh(520, 844);
    const auto start = fresh.settle(capture, inputs, 1);
    keep(start, "atlas-fresh-520x844-1x");
    const double share = differingShare(grown, start);
    std::println("grown and fresh differ in {:.4f} of their pixels", share);
    TEST_CHECK(share < 0.01);
  }

  // Mid-drag, the map is drawn to its edges; mid-flight and mid-fade frames are drawn too.
  void motionFrames(SceneCapture& capture, const PartInputs& inputs) {
    Pane pane(900, 600);
    const auto rest = pane.settle(capture, inputs, 1);
    const double before = drawnShare(rest, scaled(0, 0, 900, 600, 1));
    pane.dispatcher.pointerMotion(450, 400, 1);
    pane.dispatcher.pointerButton(450, 400, BTN_LEFT, true, 2, 0, false);
    GlPixels dragged;
    for (int step = 1; step <= 15; ++step) {
      pane.dispatcher.pointerMotion(450 + 10.0F * static_cast<float>(step), 400, 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      dragged = pane.frame(capture, inputs, 1);
    }
    keep(dragged, "atlas-pan-900x600-1x");
    pillsUnderTheirLabels(pane.view->shapeView());
    const double during = drawnShare(dragged, scaled(0, 0, 900, 600, 1));
    std::println("drawn at rest {:.3f}, mid-pan {:.3f}", before, during);
    TEST_CHECK(during > 0.3);
    pane.dispatcher.pointerButton(600, 400, BTN_LEFT, false, 3, 0, false);
    pane.settle(capture, inputs, 1);

    // Into src, two levels ahead of nothing: a flight from where src was drawn.
    auto tree = *inputs.snapshot.tree;
    const auto proj = tree.root;
    const NodeId src = tree.children[tree.nodes[proj].firstChild];
    tree.root = src;
    tree.ancestors = parentChain(tree, src);
    const auto entered = inputsFor(tree, 2);
    const auto flightStart = Clock::now();
    GlPixels flight;
    for (bool shown = false; !shown || Clock::now() - flightStart < std::chrono::milliseconds(150);) {
      TEST_CHECK(Clock::now() - flightStart < std::chrono::seconds(5));
      std::this_thread::sleep_for(std::chrono::milliseconds(4));
      flight = pane.frame(capture, entered, 1);
      shown = shown || pane.view->shownSnapshot().serial == 2;
    }
    keep(flight, "atlas-flight-900x600-1x");
    TEST_CHECK(drawnShare(flight, scaled(0, 0, 900, 600, 1)) > 0.3);
    pane.settle(capture, entered, 1);

    // Back to the top, three levels away: the old picture fades over the new one at rest.
    tree.root = 0;
    tree.ancestors.clear();
    const auto far = inputsFor(tree, 3);
    GlPixels fading;
    for (int frame = 0; frame < 2000 && pane.view->shownSnapshot().serial != 3; ++frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      fading = pane.frame(capture, far, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    fading = pane.frame(capture, far, 1);
    keep(fading, "atlas-fade-900x600-1x");
    TEST_CHECK(pane.view->shownSnapshot().serial == 3);
    pane.settle(capture, far, 1);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  const auto inputs = inputsFor(projectTree(), 1);
  smallPane(*capture, inputs);
  largePane(*capture, inputs);
  grownPane(*capture, inputs);
  motionFrames(*capture, inputs);
  std::println("atlas-view-pixels: all checks passed");
  return 0;
}
