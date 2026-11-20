#include "formats/mesh.h"
#include "metrics_renderer.h"
#include "png_writer.h"
#include "render/scene/input_dispatcher.h"
#include "scene_capture.h"
#include "test_check.h"
#include "thumbnails/mesh_preview.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/mesh_view.h"
#include "ui/palette.h"

#include <GLES2/gl2.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>
#include <print>
#include <string>
#include <wayland-client-protocol.h>

namespace {
  using luciole::MeshGeometry;
  using luciole::formats::Mesh;
  using luciole::formats::Vec3;
  using test::GlPixels;

  constexpr int kWidth = 360;
  constexpr int kHeight = 240;

  // Measures like MetricsRenderer; its texture manager is the capture's, which makes the capture's context current.
  class CaptureContext final : public Renderer {
  public:
    explicit CaptureContext(test::SceneCapture& capture) : m_capture(capture) {}
    TextMetrics measureText(
        std::string_view text, float size, const TextFont& font, float width, int lines, TextAlign align,
        TextEllipsize ellipsize, bool markup
    ) override {
      return m_metrics.measureText(text, size, font, width, lines, align, ellipsize, markup);
    }
    TextMetrics measureFont(float size, const TextFont& font) override { return m_metrics.measureFont(size, font); }
    TextMetrics measureGlyph(char32_t glyph, float size) override { return m_metrics.measureGlyph(glyph, size); }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextureManager& textureManager() override { return m_capture.textureManager(); }
    float renderScale() const noexcept override { return 1; }

  private:
    test::SceneCapture& m_capture;
    test::MetricsRenderer m_metrics;
  };

  std::shared_ptr<const MeshGeometry> cube() {
    Mesh mesh;
    for (int bits = 0; bits < 8; ++bits) {
      mesh.positions.push_back(
          Vec3{static_cast<float>(bits & 1), static_cast<float>((bits >> 1) & 1), static_cast<float>((bits >> 2) & 1)}
      );
    }
    mesh.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                    2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    luciole::formats::finishMesh(mesh);
    auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), luciole::kMeshPreviewLimits);
    TEST_CHECK(geometry.has_value());
    return std::make_shared<const MeshGeometry>(std::move(*geometry));
  }

  void usePalette(Color key, Color fill) {
    Palette colors = palette;
    colors.primary = key;
    colors.tertiary = fill;
    colors.onSurface = rgba(0.9F, 0.9F, 0.9F, 1);
    colors.onSurfaceVariant = rgba(0.8F, 0.8F, 0.8F, 1);
    setPalette(colors);
  }

  struct Rig {
    test::SceneCapture& capture;
    ::Node root;
    MeshView* view = nullptr;
    InputDispatcher dispatcher;
    std::uint32_t serial = 0;

    explicit Rig(test::SceneCapture& c) : capture(c) {
      view = static_cast<MeshView*>(root.addChild(std::make_unique<MeshView>()));
      dispatcher.setSceneRoot(&root);
    }

    GlPixels frame(float scale = 1) {
      view->setFrameSize(kWidth, kHeight);
      return capture.capture(root, kWidth, kHeight, scale);
    }

    std::uint32_t next() { return ++serial; }

    void drag(float x, float y, float dx, float dy) {
      dispatcher.pointerMotion(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, next(), 0, false);
      dispatcher.pointerMotion(x + (dx / 2), y + (dy / 2), ++serial);
      dispatcher.pointerMotion(x + dx, y + dy, ++serial);
      dispatcher.pointerButton(x + dx, y + dy, BTN_LEFT, false, next(), 0, false);
    }

    void click(float x, float y) {
      dispatcher.pointerMotion(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, next(), 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, next(), 0, false);
    }
  };

  int opaqueCount(const GlPixels& pixels) {
    int count = 0;
    for (std::size_t i = 3; i < pixels.rgba.size(); i += 4) {
      count += pixels.rgba[i] == 255 ? 1 : 0;
    }
    return count;
  }

  int differing(const GlPixels& a, const GlPixels& b) {
    TEST_CHECK(a.rgba.size() == b.rgba.size());
    int count = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
      count += a.rgba[i] != b.rgba[i] || a.rgba[i + 2] != b.rgba[i + 2] || a.rgba[i + 3] != b.rgba[i + 3] ? 1 : 0;
    }
    return count;
  }

  void save(const GlPixels& pixels, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("mesh-view-{}.png", name);
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  void showsTheModel(Rig& rig) {
    const GlPixels pixels = rig.frame();
    save(pixels, "cube-1x");
    const auto center = pixels.at(kWidth / 2, kHeight / 2);
    TEST_CHECK(center[3] == 255 && (center[0] > 40 || center[2] > 40));
    TEST_CHECK(pixels.at(2, kHeight - 3)[3] == 0);
    TEST_CHECK(opaqueCount(pixels) > kWidth * kHeight / 10);
    TEST_CHECK(rig.view->renders() == 1);
    // Frames that change nothing show the same image without drawing it again.
    (void)rig.frame();
    TEST_CHECK(rig.view->renders() == 1);
    const GlPixels scaled = rig.frame(1.5F);
    save(scaled, "cube-1.5x");
    TEST_CHECK(rig.view->renders() == 2 && scaled.width == 540);
    TEST_CHECK(rig.view->gpuBytes() >= std::size_t{540} * 360 * 6);
  }

  void dragTurns(Rig& rig) {
    const GlPixels before = rig.frame();
    const MeshOrbit start = rig.view->orbit();
    const std::uint64_t renders = rig.view->renders();
    rig.drag(100, 120, 40, 0);
    TEST_NEAR(rig.view->orbit().yaw, start.yaw + (40 * MeshView::kTurnPerPixel), 1e-4F);
    TEST_NEAR(rig.view->orbit().pitch, start.pitch, 1e-6F);
    const GlPixels turned = rig.frame();
    save(turned, "turned");
    TEST_CHECK(rig.view->renders() == renders + 1);
    TEST_CHECK(differing(before, turned) > 500);
    rig.drag(100, 120, 0, 1000);
    TEST_NEAR(rig.view->orbit().pitch, MeshOrbit::kMaxPitch, 1e-6F);
  }

  void wheelZooms(Rig& rig) {
    rig.view->resetOrbit();
    const int before = opaqueCount(rig.frame());
    for (int notch = 0; notch < 2; ++notch) {
      TEST_CHECK(rig.dispatcher.pointerAxis(180, 120, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, -15, -1, -120, -1));
    }
    TEST_NEAR(rig.view->orbit().zoom, std::exp(2 * 56 * MeshView::kZoomPerPixel), 1e-3F);
    const GlPixels closer = rig.frame();
    save(closer, "zoomed");
    TEST_CHECK(opaqueCount(closer) > before * 1.15);
    TEST_CHECK(!rig.dispatcher.pointerAxis(180, 120, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0, 15, 1, 120, 1));
    for (int i = 0; i < 200; ++i) {
      (void)rig.dispatcher.pointerAxis(180, 120, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, 15, 1, 120, 1);
    }
    TEST_NEAR(rig.view->orbit().zoom, MeshOrbit::kMinZoom, 1e-6F);
  }

  void resetButtonReturns(Rig& rig) {
    rig.view->resetOrbit();
    const GlPixels first = rig.frame();
    rig.drag(100, 120, -70, 30);
    (void)rig.frame();
    TEST_CHECK(!(rig.view->orbit() == MeshOrbit{}));
    const Button& reset = rig.view->resetButton();
    float x = 0;
    float y = 0;
    Node::absolutePosition(&reset, x, y);
    TEST_CHECK(x + reset.width() <= kWidth && y >= 0 && reset.width() > 0);
    TEST_CHECK(x > kWidth / 2.0F && y < kHeight / 4.0F);
    rig.click(x + (reset.width() / 2), y + (reset.height() / 2));
    TEST_CHECK((rig.view->orbit() == MeshOrbit{}));
    // Off the button, so its hover fill is gone.
    rig.dispatcher.pointerMotion(20, kHeight - 20, rig.next());
    const GlPixels again = rig.frame();
    TEST_CHECK(opaqueCount(again) == opaqueCount(first));
    TEST_CHECK(again.at(kWidth / 2, kHeight / 2) == first.at(kWidth / 2, kHeight / 2));
  }

  void paletteRedraws(Rig& rig) {
    const GlPixels before = rig.frame();
    const std::uint64_t renders = rig.view->renders();
    rig.root.clearDirty();
    TEST_CHECK(!rig.view->paintDirty());
    usePalette(rgba(0.2F, 0.9F, 0.3F, 1), rgba(0.9F, 0.8F, 0.1F, 1));
    TEST_CHECK(rig.view->paintDirty());
    const GlPixels after = rig.frame();
    TEST_CHECK(rig.view->renders() == renders + 1);
    TEST_CHECK(after.at(kWidth / 2, kHeight / 2)[1] > before.at(kWidth / 2, kHeight / 2)[1] + 40);
    usePalette(rgba(0.85F, 0.45F, 0.35F, 1), rgba(0.30F, 0.45F, 0.90F, 1));
  }

  // A torus around the z axis with one smooth normal per vertex: ring radius 1, tube radius 0.35.
  std::shared_ptr<const MeshGeometry> torus() {
    constexpr std::uint32_t kRing = 48;
    constexpr std::uint32_t kTube = 24;
    Mesh mesh;
    for (std::uint32_t u = 0; u < kRing; ++u) {
      const double a = 2 * std::numbers::pi * u / kRing;
      for (std::uint32_t v = 0; v < kTube; ++v) {
        const double b = 2 * std::numbers::pi * v / kTube;
        const auto nx = static_cast<float>(std::cos(b) * std::cos(a));
        const auto ny = static_cast<float>(std::cos(b) * std::sin(a));
        const auto nz = static_cast<float>(std::sin(b));
        mesh.positions.push_back(
            Vec3{
                static_cast<float>(std::cos(a)) + (0.35F * nx), static_cast<float>(std::sin(a)) + (0.35F * ny),
                0.35F * nz
            }
        );
        mesh.normals.push_back(Vec3{nx, ny, nz});
      }
    }
    for (std::uint32_t u = 0; u < kRing; ++u) {
      for (std::uint32_t v = 0; v < kTube; ++v) {
        const std::uint32_t a = (u * kTube) + v;
        const std::uint32_t b = (((u + 1) % kRing) * kTube) + v;
        const std::uint32_t c = (((u + 1) % kRing) * kTube) + ((v + 1) % kTube);
        const std::uint32_t d = (u * kTube) + ((v + 1) % kTube);
        for (const std::uint32_t index : {a, b, c, a, c, d}) {
          mesh.indices.push_back(index);
        }
      }
    }
    luciole::formats::finishMesh(mesh);
    auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), luciole::kMeshPreviewLimits);
    TEST_CHECK(geometry.has_value() && !geometry->flatNormals);
    return std::make_shared<const MeshGeometry>(std::move(*geometry));
  }

  // The default dark Workbench colors over a surface backdrop: the hole of the torus shows the backdrop.
  void inTheme(test::SceneCapture& capture) {
    const auto color = [](std::uint32_t rgb) {
      return rgba(
          static_cast<float>((rgb >> 16) & 0xFFU) / 255, static_cast<float>((rgb >> 8) & 0xFFU) / 255,
          static_cast<float>(rgb & 0xFFU) / 255, 1
      );
    };
    Palette colors = palette;
    colors.surface = color(0x15162E);
    colors.onSurface = color(0xECE9F4);
    colors.onSurfaceVariant = color(0xA5A7C6);
    colors.primary = color(0xF7E9A8);
    colors.tertiary = color(0xA0DDC0);
    setPalette(colors);
    ::Node root;
    auto* backdrop = static_cast<Box*>(root.addChild(std::make_unique<Box>()));
    backdrop->setFill(colorSpecFromRole(ColorRole::Surface));
    backdrop->setFrameSize(kWidth, kHeight);
    auto* view = static_cast<MeshView*>(root.addChild(std::make_unique<MeshView>()));
    view->setFrameSize(kWidth, kHeight);
    view->setGeometry(torus());
    const GlPixels pixels = capture.capture(root, kWidth, kHeight, 1);
    save(pixels, "torus-dark");
    const auto hole = pixels.at(kWidth / 2, kHeight / 2);
    TEST_CHECK(hole[0] == 0x15 && hole[1] == 0x16 && hole[2] == 0x2E && hole[3] == 255);
    int lit = 0;
    for (int x = 0; x < kWidth; ++x) {
      const auto pixel = pixels.at(x, kHeight / 2);
      lit += pixel[0] > 0x40 || pixel[1] > 0x40 ? 1 : 0;
    }
    TEST_CHECK(lit > kWidth / 8);
    CaptureContext context(capture);
    view->release(context);
  }

  void releaseAndHide(Rig& rig, test::SceneCapture& capture) {
    (void)rig.frame();
    CaptureContext context(capture);
    rig.view->release(context);
    TEST_CHECK(rig.view->gpuBytes() == 0);
    const std::uint64_t renders = rig.view->renders();
    (void)rig.frame();
    TEST_CHECK(rig.view->renders() == renders + 1 && rig.view->gpuBytes() > 0);
    // A hidden view is not drawn, so it costs nothing even when its camera changes.
    rig.view->setVisible(false);
    rig.view->resetOrbit();
    rig.view->setGeometry(cube());
    const GlPixels hidden = rig.frame();
    TEST_CHECK(rig.view->renders() == renders + 1);
    TEST_CHECK(opaqueCount(hidden) == 0);
    rig.view->setVisible(true);
    rig.view->release(context);
    rig.view->setGeometry(nullptr);
    const GlPixels empty = rig.frame();
    TEST_CHECK(empty.at(kWidth / 2, kHeight / 2)[3] == 0 && rig.view->gpuBytes() == 0);
  }

  // A drawn view dropped without release frees its image and buffers as it goes, with no frame after it.
  void droppedViewFreesItsGpu(test::SceneCapture& capture) {
    std::weak_ptr<const MeshGpu> watched;
    GLuint image = 0;
    {
      ::Node root;
      auto* view = static_cast<MeshView*>(root.addChild(std::make_unique<MeshView>()));
      view->setFrameSize(kWidth, kHeight);
      view->setGeometry(cube());
      (void)capture.capture(root, kWidth, kHeight, 1);
      watched = view->gpu();
      const auto drawn = watched.lock();
      TEST_CHECK(drawn != nullptr && drawn->bytes() > 0 && drawn->image().valid());
      image = drawn->image().value();
      TEST_CHECK(glIsTexture(image) == GL_TRUE);
    }
    const auto dropped = watched.lock();
    TEST_CHECK(dropped == nullptr || dropped->bytes() == 0);
    TEST_CHECK(glIsTexture(image) == GL_FALSE);
  }

  // How far the camera stands from the cube's center, which is at 0.5 on each axis.
  float cameraDistance(const MeshView& view) {
    const auto& m = view.camera(1.5F).modelView;
    return -((m[2] + m[6] + m[10]) * 0.5F + m[14]);
  }

  // The preview size brings the camera nearer or takes it farther, on top of the wheel's zoom.
  void previewScaleMovesTheCamera(Rig& rig) {
    rig.view->resetOrbit();
    rig.view->setPreviewScale(1);
    const float fitted = cameraDistance(*rig.view);
    TEST_CHECK(fitted > 0);
    int previous = 0;
    for (const float scale : {0.7F, 1.0F, 1.5F, 3.0F}) {
      rig.view->setPreviewScale(scale);
      TEST_CHECK(rig.view->previewScale() == scale);
      TEST_NEAR(cameraDistance(*rig.view), fitted / scale, fitted * 1e-4F);
      const GlPixels pixels = rig.frame();
      save(pixels, std::format("scale-{}", static_cast<int>(std::lround(scale * 100))));
      const int drawn = opaqueCount(pixels);
      std::println("model at {}%: {} pixels drawn", static_cast<int>(std::lround(scale * 100)), drawn);
      TEST_CHECK(drawn > previous);
      previous = drawn;
    }
    for (const float bad : {0.0F, -2.0F, std::numeric_limits<float>::infinity()}) {
      rig.view->setPreviewScale(bad);
      TEST_CHECK(rig.view->previewScale() == 3.0F);
    }
    rig.view->setPreviewScale(1);
  }
} // namespace

int main() {
  std::string reason;
  const auto capture = test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  usePalette(rgba(0.85F, 0.45F, 0.35F, 1), rgba(0.30F, 0.45F, 0.90F, 1));
  {
    Rig rig(*capture);
    rig.view->setGeometry(cube());
    TEST_CHECK((rig.view->orbit() == MeshOrbit{}));
    showsTheModel(rig);
    dragTurns(rig);
    wheelZooms(rig);
    resetButtonReturns(rig);
    paletteRedraws(rig);
    previewScaleMovesTheCamera(rig);
    releaseAndHide(rig, *capture);
  }
  inTheme(*capture);
  droppedViewFreesItsGpu(*capture);
  std::println("mesh view checks passed");
}
