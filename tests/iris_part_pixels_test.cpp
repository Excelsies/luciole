#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "render/scene/text_node.h"
#include "scene_capture.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/shape_view.h"
#include "ui/workbench/instrument_view.h"
#include "workbench/iris_hit.h"
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
#include <numbers>
#include <print>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

// The Iris instrument drawn offscreen with real text: the ancestor stack, the hub's lines and trail chips, the
// readout, the inner band and the row icons, as a compact pane and as a full dial.
namespace {
  using namespace luciole;
  using namespace luciole::workbench;
  using luciole::test::WorkbenchRow;
  using ::test::GlPixels;

  const ColorPalette kPalette{};
  constexpr float kCompactWidth = 340;
  constexpr float kCompactHeight = 534;
  constexpr float kFullWidth = 900;
  constexpr float kFullHeight = 600;

  struct Ids {
    NodeId proj = 0;
    NodeId assets = 0;
    NodeId sounds = 0;
  };

  // / > home > user > proj: five folders and 30 small notes, so a full dial needs "+N more"; assets holds folders.
  Tree fixture(Ids& ids) {
    // Rows name their entries by view, so the names stay put until the tree has copied them.
    std::vector<std::string> names;
    names.reserve(64);
    std::vector<WorkbenchRow> rows;
    const auto add = [&](int parent, std::string name, bool folder, double bytes, double items) {
      names.push_back(std::move(name));
      rows.push_back({.parent = parent, .name = {}, .folder = folder, .bytes = bytes, .items = items, .files = items});
      return static_cast<int>(rows.size()) - 1;
    };
    const int top = add(-1, "/", true, 9e6, 4);
    const int home = add(top, "home", true, 4e6, 3);
    add(top, "usr", true, 3e6, 0);
    add(top, "etc", true, 1e6, 0);
    const int user = add(home, "user", true, 3e6, 3);
    add(home, "guest", true, 6e5, 0);
    const int proj = add(user, "proj", true, 4e5, 35);
    add(user, "music", true, 2e6, 0);
    const int assets = add(proj, "assets", true, 171e3, 5);
    for (const auto& [name, bytes] : {std::pair{"build", 78e3}, {"docs", 47e3}, {"src", 43e3}, {"tests", 20e3}}) {
      add(proj, name, true, bytes, 2);
    }
    for (int i = 0; i < 30; ++i) {
      add(proj, std::format("note-{:02}.txt", i), false, 400.0 + i * 10, 0);
    }
    const int sounds = add(assets, "sounds", true, 54e3, 2);
    add(assets, "img", true, 41e3, 0);
    add(assets, "fonts", true, 30e3, 0);
    add(assets, "logo.png", false, 26e3, 0);
    add(assets, "icon.svg", false, 20e3, 0);
    add(sounds, "loops", true, 24e3, 0);
    add(sounds, "beep.wav", false, 30e3, 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      rows[i].name = names[i];
    }
    auto tree = luciole::test::workbenchTree(rows, 1e6);
    ids = {
        .proj = static_cast<NodeId>(proj), .assets = static_cast<NodeId>(assets), .sounds = static_cast<NodeId>(sounds)
    };
    return tree;
  }

  PartInputs openedAt(Tree tree, NodeId folder, std::uint64_t serial) {
    tree.root = folder;
    tree.ancestors = parentChain(tree, folder);
    return {
        .snapshot = {.tree = std::make_shared<const Tree>(std::move(tree)), .serial = serial},
        .settings = {},
        .palette = kPalette
    };
  }

  Color color(Rgb rgb) {
    return rgba(
        static_cast<float>(rgb.r) / 255.0F, static_cast<float>(rgb.g) / 255.0F, static_cast<float>(rgb.b) / 255.0F, 1
    );
  }

  struct Rig {
    ::Node root;
    InstrumentView* view = nullptr;
    InputDispatcher dispatcher;
    float width = 0;
    float height = 0;

    Rig(float w, float h) : width(w), height(h) {
      auto* surface = static_cast<Box*>(root.addChild(std::make_unique<Box>()));
      surface->setSize(w, h);
      surface->setFill(color(kPalette.surface));
      view = static_cast<InstrumentView*>(root.addChild(std::make_unique<InstrumentView>(WorkbenchInstrument::Iris, [] {
        return 0U;
      })));
      view->setSize(w, h);
      dispatcher.setSceneRoot(&root);
    }

    // Frames until the view shows `inputs`, drawing each one; returns the last capture.
    GlPixels settle(::test::SceneCapture& capture, const PartInputs& inputs, float scale) {
      GlPixels pixels;
      for (int frame = 0; frame < 8; ++frame) {
        view->update(inputs, false);
        pixels = capture.capture(root, static_cast<int>(width), static_cast<int>(height), scale);
      }
      TEST_CHECK(view->shownSnapshot().tree == inputs.snapshot.tree);
      return pixels;
    }

    const Shape* shape(NodeId node, SceneRole role) const {
      for (const auto& each : view->shapeView().shapes()) {
        if (each.id.node == node && sceneRole(each.id.role) == role && each.hittable) {
          return &each;
        }
      }
      return nullptr;
    }

    const Shape* chip(std::size_t index) const {
      for (const auto& each : view->shapeView().shapes()) {
        if (each.id == iris::chipId(index) && each.hittable) {
          return &each;
        }
      }
      return nullptr;
    }

    void pointAt(float x, float y) { dispatcher.pointerMotion(x, y, 1); }

    void key(std::uint32_t sym) {
      dispatcher.setFocus(&view->shapeView().inputArea());
      dispatcher.keyEvent(sym, 0, 0, true);
      dispatcher.keyEvent(sym, 0, 0, false);
    }
  };

  std::array<int, 4> pixelAt(const GlPixels& pixels, float x, float y, float scale) {
    const auto p = pixels.at(static_cast<int>(std::floor(x * scale)), static_cast<int>(std::floor(y * scale)));
    return {p[0], p[1], p[2], p[3]};
  }

  bool near(const std::array<int, 4>& actual, Rgb expected, int tolerance) {
    const bool close = std::abs(actual[0] - expected.r) <= tolerance
        && std::abs(actual[1] - expected.g) <= tolerance
        && std::abs(actual[2] - expected.b) <= tolerance;
    if (!close) {
      std::println(
          stderr, "pixel {} {} {}, expected {} {} {}", actual[0], actual[1], actual[2], expected.r, expected.g,
          expected.b
      );
    }
    return close;
  }

  // Whether a device pixel differs from the surface: something was drawn there.
  bool inkedAt(const GlPixels& pixels, int x, int y) {
    const auto p = pixels.at(x, y);
    const int distance =
        std::abs(p[0] - kPalette.surface.r) + std::abs(p[1] - kPalette.surface.g) + std::abs(p[2] - kPalette.surface.b);
    return distance > 24;
  }

  // Device pixels in a logical box that differ from the surface: what was drawn there.
  std::size_t inked(const GlPixels& pixels, ShapeRect box, float scale) {
    std::size_t count = 0;
    const int left = std::max(0, static_cast<int>(std::ceil(box.x * scale)));
    const int top = std::max(0, static_cast<int>(std::ceil(box.y * scale)));
    const int right = std::min(pixels.width, static_cast<int>(std::floor((box.x + box.width) * scale)));
    const int bottom = std::min(pixels.height, static_cast<int>(std::floor((box.y + box.height) * scale)));
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        count += inkedAt(pixels, x, y) ? 1U : 0U;
      }
    }
    return count;
  }

  void keep(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("iris-{}.png", name);
    TEST_CHECK(::test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // The compact pane: centered ancestor rows, the hub's lines and chips, the readout; a hovered row lights up its
  // own width, and a hovered folder arc gets its inner band.
  void compactPane(::test::SceneCapture& capture, float scale) {
    Ids ids;
    const auto tree = fixture(ids);
    Rig rig(kCompactWidth, kCompactHeight);
    rig.settle(capture, openedAt(tree, ids.sounds, 1), scale);
    const auto inputs = openedAt(tree, ids.proj, 2);
    const auto idle = rig.settle(capture, inputs, scale);
    keep(idle, std::format("compact-{}x", scale));
    const float middle = kCompactWidth / 2;
    std::size_t rows = 0;
    for (const auto& shape : rig.view->shapeView().shapes()) {
      if (sceneRole(shape.id.role) != SceneRole::Stack) {
        continue;
      }
      ++rows;
      const auto& box = shape.rect;
      TEST_NEAR(box.x + box.width / 2, middle, 0.5F);
      TEST_CHECK(box.width < kCompactWidth / 2);
      TEST_CHECK(inked(idle, box, scale) > 10);
      // Nothing is written beside a row.
      TEST_CHECK(inked(idle, {.x = 4, .y = box.y, .width = box.x - 8, .height = box.height, .radius = 0}, scale) == 0);
    }
    TEST_CHECK(rows == 3);
    // Three rings leave a hub of 50 px, too small for the trail.
    TEST_CHECK(std::ranges::none_of(rig.view->shapeView().shapes(), [](const Shape& shape) {
      return sceneRole(shape.id.role) == SceneRole::Chip;
    }));
    const ShapeRect readout{.x = 6, .y = kCompactHeight - 22, .width = kCompactWidth - 12, .height = 18, .radius = 0};
    TEST_CHECK(inked(idle, readout, scale) > 30);
    // A hovered ancestor row (user, drawn at full strength): its own background, nothing beside it.
    const auto* user = rig.shape(4, SceneRole::Stack);
    TEST_CHECK(user != nullptr);
    const auto row = user->rect;
    rig.pointAt(row.x + row.width / 2, row.y + row.height / 2);
    const auto hovered = rig.settle(capture, inputs, scale);
    keep(hovered, std::format("compact-stack-hover-{}x", scale));
    TEST_CHECK(near(pixelAt(hovered, row.x + 1.5F, row.y + row.height / 2, scale), kPalette.surfaceContainerHigh, 4));
    TEST_CHECK(near(pixelAt(hovered, row.x - 3, row.y + row.height / 2, scale), kPalette.surface, 2));
    // A hovered folder arc shows its entries along the inner edge, and the readout names it.
    const auto* arc = rig.shape(ids.assets, SceneRole::Piece);
    TEST_CHECK(arc != nullptr && arc->kind == ShapeKind::Sector);
    const auto& s = arc->sector;
    const float angle = (s.a0 + s.a1) / 2 * std::numbers::pi_v<float> / 180;
    const auto at = [&](float radius) {
      return std::pair{s.center.x + radius * std::sin(angle), s.center.y - radius * std::cos(angle)};
    };
    const auto [bandX, bandY] = at(s.r0 + 9);
    const auto [arcX, arcY] = at((s.r0 + s.r1) / 2 + 12);
    rig.pointAt(arcX, arcY);
    const auto band = rig.settle(capture, inputs, scale);
    keep(band, std::format("compact-inner-band-{}x", scale));
    TEST_CHECK(pixelAt(band, bandX, bandY, scale) != pixelAt(idle, bandX, bandY, scale));
    TEST_CHECK(inked(band, {.x = bandX - 1, .y = bandY - 1, .width = 2, .height = 2, .radius = 0}, scale) > 0);
    std::size_t changed = 0;
    for (float x = readout.x; x < readout.x + readout.width; x += 1) {
      changed += pixelAt(band, x, readout.y + 9, scale) != pixelAt(idle, x, readout.y + 9, scale) ? 1U : 0U;
    }
    TEST_CHECK(changed > 10);
  }

  // The full dial: trail chips back to where the view was, row names with their icons, and End resting the
  // cursor on "+N more".
  void fullDial(::test::SceneCapture& capture, float scale) {
    Ids ids;
    const auto tree = fixture(ids);
    Rig rig(kFullWidth, kFullHeight);
    rig.settle(capture, openedAt(tree, ids.sounds, 1), scale);
    auto inputs = openedAt(tree, ids.proj, 2);
    inputs.trail.folders = {ids.assets, ids.sounds};
    const auto idle = rig.settle(capture, inputs, scale);
    keep(idle, std::format("full-{}x", scale));
    // The trail: assets, then sounds.
    for (const std::size_t index : {0U, 1U}) {
      const auto* chip = rig.chip(index);
      TEST_CHECK(chip != nullptr);
      const auto& box = chip->rect;
      TEST_CHECK(near(pixelAt(idle, box.x + 3, box.y + box.height / 2, scale), kPalette.surfaceContainerHigh, 3));
      TEST_CHECK(inked(idle, box, scale) > 20);
    }
    // Only the chip under the pointer lights up.
    const auto first = rig.chip(0)->rect;
    const auto second = rig.chip(1)->rect;
    rig.pointAt(first.x + first.width / 2, first.y + first.height / 2);
    const auto chipHover = rig.settle(capture, inputs, scale);
    keep(chipHover, std::format("full-chip-hover-{}x", scale));
    const float firstMiddle = first.y + first.height / 2;
    const float secondMiddle = second.y + second.height / 2;
    TEST_CHECK(near(pixelAt(chipHover, first.x + 3, firstMiddle, scale), kPalette.surfaceContainerHighest, 3));
    TEST_CHECK(near(pixelAt(chipHover, second.x + 3, secondMiddle, scale), kPalette.surfaceContainerHigh, 3));
    rig.pointAt(2, kFullHeight - 2);
    std::size_t icons = 0;
    for (const auto& child : rig.view->shapeView().labelLayer().children()) {
      const auto* label = dynamic_cast<const TextNode*>(child.get());
      if (label == nullptr || !label->visible() || label->text() != "assets") {
        continue;
      }
      float x = 0;
      float y = 0;
      ::Node::absolutePosition(label, x, y);
      // The icon sits 26 px left of the name, 18 px wide.
      TEST_CHECK(inked(idle, {.x = x - 26, .y = y - 16, .width = 18, .height = 20, .radius = 0}, scale) > 20);
      ++icons;
    }
    TEST_CHECK(icons == 1);
    rig.key(XKB_KEY_End);
    const auto more = rig.settle(capture, inputs, scale);
    keep(more, std::format("full-more-cursor-{}x", scale));
    const auto* row = rig.shape(ids.proj, SceneRole::Row);
    TEST_CHECK(row != nullptr);
    TEST_CHECK(near(
        pixelAt(more, row->rect.x + 3, row->rect.y + row->rect.height / 2, scale), kPalette.surfaceContainerHigh, 4
    ));
    TEST_CHECK(
        near(pixelAt(more, row->rect.x + 0.5F, row->rect.y + row->rect.height / 2, scale), kPalette.primary, 40)
    );
  }

  // / > home > box, where box holds only two hidden files: its note wraps inside the hub of a small pane, shrinking
  // where its lines would leave the hub, and leaves the empty main ring around the hub blank.
  void notesWrapInSmallPanes(::test::SceneCapture& capture, float scale) {
    const auto tree = luciole::test::workbenchTree(
        {{.parent = -1, .name = "/", .folder = true, .bytes = 3e3, .items = 1},
         {.parent = 0, .name = "home", .folder = true, .bytes = 3e3, .items = 1},
         {.parent = 1, .name = "box", .folder = true, .bytes = 3e3, .items = 2},
         {.parent = 2, .name = ".a", .bytes = 2e3},
         {.parent = 2, .name = ".b", .bytes = 1e3}},
        1e6
    );
    struct Pane {
      float width = 0;
      float height = 0;
      bool inside = false; // the note fits the hub whole, so nothing at all is drawn around it
    };
    for (const auto& [width, height, inside] : {Pane{320, 380, true}, Pane{300, 330, false}, Pane{260, 240, false}}) {
      Rig rig(width, height);
      const auto pixels = rig.settle(capture, openedAt(tree, 2, 1), scale);
      keep(pixels, std::format("note-{}x{}-{}x", width, height, scale));
      std::string written;
      for (const auto& child : rig.view->shapeView().labelLayer().children()) {
        const auto* label = dynamic_cast<const TextNode*>(child.get());
        if (label != nullptr && label->visible()) {
          written += label->text() + "\n";
          // A note that fits the hub is whole; a smaller hub ends it in an ellipsis.
          TEST_CHECK(!inside || !label->text().contains("…"));
        }
      }
      TEST_CHECK(!inside || (written.contains("hidden") && written.contains("them)")));
      // Smaller hubs keep the note whole or end it in an ellipsis, never cut in the middle of a line.
      TEST_CHECK(!written.contains("Only") || written.contains("them)") || written.contains("…"));
      const auto* hub = rig.shape(2, SceneRole::Hub);
      TEST_CHECK(hub != nullptr);
      float ring = width;
      for (const auto& shape : rig.view->shapeView().shapes()) {
        if (shape.kind == ShapeKind::Sector) {
          ring = std::min(ring, shape.sector.r0);
        }
      }
      const auto center = hub->circle.center;
      const float radius = hub->circle.radius;
      TEST_CHECK(ring > radius + 10 && ring < width);
      // Device pixels in the empty main ring that differ from the surface: beside the hub's body, and anywhere
      // around the hub where the note fits it.
      std::size_t beside = 0;
      std::size_t around = 0;
      for (int y = 0; y < pixels.height; ++y) {
        for (int x = 0; x < pixels.width; ++x) {
          const float dx = (static_cast<float>(x) + 0.5F) / scale - center.x;
          const float dy = (static_cast<float>(y) + 0.5F) / scale - center.y;
          const float distance = std::hypot(dx, dy);
          const bool ink = distance > radius + 2 && distance < ring - 2 && inkedAt(pixels, x, y);
          beside += ink && std::abs(dx) > radius ? 1U : 0U;
          around += ink ? 1U : 0U;
        }
      }
      std::println("{}x{} at {}x: hub {:.0f} px, {} pixels around it", width, height, scale, radius, around);
      TEST_CHECK(beside == 0 && (!inside || around == 0));
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
  for (const float scale : {1.0F, 2.0F}) {
    compactPane(*capture, scale);
    fullDial(*capture, scale);
    notesWrapInSmallPanes(*capture, scale);
  }
  std::println("iris-part-pixels: all checks passed");
  return 0;
}
