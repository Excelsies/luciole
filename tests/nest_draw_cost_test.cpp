#include "gl_test_context.h"
#include "png_writer.h"
#include "render/backend/render_backend.h"
#include "test_check.h"
#include "ui/controls/shape_batch_node.h"
#include "workbench/color.h"
#include "workbench/nest_scene.h"
#include "workbench/nest_texture.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench/shape_mesh.h"
#include "workbench/tree.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

// What drawing the largest Nest frame the tests use costs: a folder of 10,000 entries at 2560 by 1400 with its tile
// texture, a linked tile's glow and an open peek card with its shadow. Reports the shapes of the frame and of the
// glow and shadow, and the batch's draw time.
namespace {
  using namespace luciole::workbench;
  using test::GlPixels;

  constexpr std::size_t kEntries = 10000;
  constexpr int kWidth = 2560;
  constexpr int kHeight = 1400;
  constexpr int kDraws = 61;

  // top > mid > here; here holds kEntries entries, every `folderEvery`-th one (none for 0) a folder of `files` files.
  Tree tenThousand(std::size_t folderEvery, std::size_t files) {
    Tree tree;
    tree.now = 1e9;
    std::vector<std::vector<NodeId>> kids;
    const auto add = [&](std::string name, NodeId parent, bool folder, double bytes) {
      luciole::workbench::Node node;
      node.name = std::move(name);
      node.parent = parent;
      node.folder = folder;
      node.bytes = bytes;
      node.modified = 1e9 - 3600;
      node.category = static_cast<std::uint8_t>(fileCategory(node.name, folder));
      tree.nodes.push_back(std::move(node));
      kids.emplace_back();
      const auto id = static_cast<NodeId>(tree.nodes.size() - 1);
      if (parent != kNoNode) {
        kids[parent].push_back(id);
      }
      return id;
    };
    const auto top = add("top", kNoNode, true, 1e12);
    const auto mid = add("mid", top, true, 1e11);
    for (std::size_t i = 0; i < 40; ++i) {
      add("s" + std::to_string(i), top, i % 3 == 0, 1e8 + static_cast<double>(i));
    }
    const auto here = add("here", mid, true, 1e10);
    for (std::size_t i = 0; i < 60; ++i) {
      add("m" + std::to_string(i), mid, i % 4 == 0, 1e7 + static_cast<double>(i));
    }
    for (std::size_t i = 0; i < kEntries; ++i) {
      const bool folder = folderEvery > 0 && i % folderEvery == 0;
      const auto id =
          add((folder ? "d" : "f") + std::to_string(i) + (folder ? "" : ".txt"), here, folder,
              1000.0 + static_cast<double>((i * 7919) % 100000));
      for (std::size_t k = 0; folder && k < files; ++k) {
        add("k" + std::to_string(k) + ".png", id, false, 10.0 + static_cast<double>((k * 31) % 997));
      }
    }
    for (std::size_t id = 0; id < tree.nodes.size(); ++id) {
      tree.nodes[id].firstChild = static_cast<std::uint32_t>(tree.children.size());
      tree.nodes[id].childCount = static_cast<std::uint32_t>(kids[id].size());
      tree.nodes[id].items = static_cast<double>(kids[id].size());
      tree.children.insert(tree.children.end(), kids[id].begin(), kids[id].end());
    }
    tree.root = here;
    tree.ancestors = parentChain(tree, here);
    return tree;
  }

  struct Frame {
    std::vector<Shape> scene;
    std::vector<Shape> peek;
    std::size_t glow = 0;   // shapes of the linked tile's glow
    std::size_t shadow = 0; // shapes under the peek card's head
  };

  // The scene with its texture, the second folder tile linked and the first one's peek open.
  Frame nestFrame(const Tree& tree) {
    const LayoutSettings settings;
    const ColorPalette palette;
    const auto scene = buildNestScene(tree, settings, kWidth, kHeight, palette);
    NestTextureSources sources;
    for (const auto folder : missingNestSources(scene, sources)) {
      sources.emplace(folder, nestTextureSource(tree, settings, folder, palette));
    }
    const auto texture = nestTexturePatches(scene, sources);
    std::vector<std::size_t> folders;
    for (std::size_t i = 0; i < scene.pieces.size() && folders.size() < 2; ++i) {
      if (scene.pieces[i].kind == NestPieceKind::Tile && nestPeekAllowed(tree, scene, i)) {
        folders.push_back(i);
      }
    }
    Frame frame;
    const NodeId linked = folders.size() > 1 ? scene.pieces[folders[1]].node : kNoNode;
    frame.scene =
        nestShapes(tree, scene, texture, {.hovered = {}, .linked = linked, .cursor = kNoNode}, palette).shapes;
    frame.glow = static_cast<std::size_t>(std::ranges::count_if(frame.scene, [&](const Shape& shape) {
      return shape.id.node == linked && sceneRole(shape.id.role) == SceneRole::Mark;
    }));
    if (!folders.empty()) {
      const auto peek = buildNestPeek(tree, settings, scene, folders[0], palette);
      if (peek) {
        frame.peek = nestPeekShapes(tree, *peek, {}, palette).shapes;
        const auto head = std::ranges::find_if(frame.peek, [](const Shape& shape) {
          return sceneRole(shape.id.role) == SceneRole::Head;
        });
        frame.shadow = static_cast<std::size_t>(head - frame.peek.begin());
      }
    }
    return frame;
  }

  void draw(RenderBackend& backend, const ShapeBatchNode& node) {
    node.draw(
        backend,
        CustomDrawContext{
            .surfaceWidth = static_cast<float>(kWidth),
            .surfaceHeight = static_cast<float>(kHeight),
            .pixelScaleX = 1,
            .pixelScaleY = 1,
        }
    );
  }

  // Median milliseconds of drawing both nodes, uploads done beforehand.
  double drawMs(
      const test::GlTestContext& gl, RenderBackend& backend, const ShapeBatchNode& scene, const ShapeBatchNode& peek
  ) {
    using Clock = std::chrono::steady_clock;
    const auto once = [&] {
      gl.clear(0.07F, 0.07F, 0.1F, 1);
      backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
      draw(backend, scene);
      draw(backend, peek);
    };
    once();
    glFinish();
    std::vector<double> samples;
    for (int run = 0; run < kDraws; ++run) {
      const auto start = Clock::now();
      once();
      glFinish();
      samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
  }

  void keep(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  void reportEachCase(const test::GlTestContext& gl, RenderBackend& backend) {
    struct Case {
      const char* name;
      std::size_t folderEvery;
      std::size_t files;
    };
    constexpr std::array<Case, 3> kCases{{
        {.name = "files only", .folderEvery = 0, .files = 0},
        {.name = "a folder in ten", .folderEvery = 10, .files = 30},
        {.name = "folders only", .folderEvery = 1, .files = 20},
    }};
    for (const auto& each : kCases) {
      const auto frame = nestFrame(tenThousand(each.folderEvery, each.files));
      ShapeBatchNode scene;
      ShapeBatchNode peek;
      scene.setShapes(frame.scene, 1);
      peek.setShapes(frame.peek, 1);
      TEST_CHECK(!scene.mesh().truncated && !peek.mesh().truncated);
      TEST_CHECK(scene.mesh().drawn + scene.mesh().skipped == frame.scene.size());
      const double ms = drawMs(gl, backend, scene, peek);
      TEST_CHECK(glGetError() == GL_NO_ERROR);
      std::println(
          "  {}: {} scene shapes and {} peek shapes ({} drawn), glow {} shapes, card shadow {} shapes, draw {:.3f} ms "
          "(median of {})",
          each.name, frame.scene.size(), frame.peek.size(), scene.mesh().drawn + peek.mesh().drawn, frame.glow,
          frame.shadow, ms, kDraws
      );
      if (each.folderEvery == 1) {
        keep(gl.readPixels(), "nest-largest-1x.png");
      }
    }
  }
} // namespace

int main() {
  std::string reason;
  const auto gl = test::GlTestContext::create(kWidth, kHeight, false, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  {
    const auto backend = createDefaultRenderBackend();
    std::println("renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    reportEachCase(*gl, *backend);
  }
  std::println("nest draw cost checks passed");
  return 0;
}
