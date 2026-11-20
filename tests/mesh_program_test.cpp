#include "formats/mesh.h"
#include "gl_test_context.h"
#include "png_writer.h"
#include "test_check.h"
#include "thumbnails/mesh_preview.h"
#include "ui/controls/mesh_program.h"
#include "ui/controls/mesh_view.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <string>
#include <vector>

namespace {
  using luciole::MeshGeometry;
  using luciole::formats::Mesh;
  using luciole::formats::Vec3;
  using test::GlPixels;

  constexpr int kWidth = 320;
  constexpr int kHeight = 240;
  const Color kRed = rgba(1, 0, 0, 1);
  const Color kBlue = rgba(0, 0, 1, 1);

  // The geometry the helper would answer for `mesh`, after the parent's checks.
  MeshGeometry geometryOf(Mesh mesh) {
    luciole::formats::finishMesh(mesh);
    auto geometry = luciole::readMeshWorkerResult(luciole::encodeMeshResult(mesh), luciole::kMeshPreviewLimits);
    TEST_CHECK(geometry.has_value());
    return std::move(*geometry);
  }

  // A unit cube with outward triangles and no normals.
  MeshGeometry cube() {
    Mesh mesh;
    for (int bits = 0; bits < 8; ++bits) {
      mesh.positions.push_back(
          Vec3{static_cast<float>(bits & 1), static_cast<float>((bits >> 1) & 1), static_cast<float>((bits >> 2) & 1)}
      );
    }
    mesh.indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                    2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    return geometryOf(std::move(mesh));
  }

  Vec3 vec(const std::array<float, 3>& v) { return Vec3{v[0], v[1], v[2]}; }

  // A square of side `side` centered on the z axis at depth `z`, all of its normals `normal`, appended to `mesh`.
  void addSquare(Mesh& mesh, float side, float z, const Vec3& normal) {
    const auto first = static_cast<std::uint32_t>(mesh.positions.size());
    const float h = side / 2;
    for (const auto& [x, y] : {std::pair{-h, -h}, {h, -h}, {-h, h}, {h, h}}) {
      mesh.positions.push_back(Vec3{x, y, z});
      mesh.normals.push_back(normal);
    }
    for (const std::uint32_t corner : {0U, 1U, 2U, 1U, 3U, 2U}) {
      mesh.indices.push_back(first + corner);
    }
  }

  // The triangle with corners (0, 0), (1, 0) and (0, 1): most of its area is low.
  MeshGeometry lowTriangle() {
    Mesh mesh;
    mesh.positions = {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
    mesh.indices = {0, 1, 2};
    return geometryOf(std::move(mesh));
  }

  MeshLook lookAt(const MeshGeometry& geometry, const MeshOrbit& orbit, int width = kWidth, int height = kHeight) {
    return MeshLook{
        .camera = meshCamera(geometry, orbit, static_cast<float>(width) / static_cast<float>(height)),
        .keyLight = kRed,
        .fillLight = kBlue,
    };
  }

  bool opaque(const std::array<std::uint8_t, 4>& pixel) { return pixel[3] == 255; }

  void save(const GlPixels& pixels, std::string_view name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / std::format("mesh-program-{}.png", name);
    TEST_CHECK(test::writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  // The image's pixels, read through a framebuffer of its own, top row first.
  GlPixels readImage(const MeshGpu& gpu) {
    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu.image().value(), 0);
    TEST_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    GlPixels pixels{.width = gpu.imageWidth(), .height = gpu.imageHeight(), .rgba = {}};
    pixels.rgba.resize(static_cast<std::size_t>(pixels.width) * static_cast<std::size_t>(pixels.height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, pixels.width, pixels.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    glDeleteFramebuffers(1, &framebuffer);
    return pixels;
  }

  // The image `program` draws of `geometry` at kWidth by kHeight, top row first.
  GlPixels renderImage(MeshProgram& program, const MeshGeometry& geometry, const MeshLook& look) {
    auto gpu = program.createGpu();
    TEST_CHECK(program.render(*gpu, geometry, 1, look, kWidth, kHeight));
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    auto pixels = readImage(*gpu);
    gpu->release();
    return pixels;
  }

  // Each lit face of a flat-shaded cube has one color of its own.
  void cubeFacesDiffer(MeshProgram& program) {
    const MeshGeometry box = cube();
    const GlPixels pixels = renderImage(program, box, lookAt(box, MeshOrbit{}));
    save(pixels, "cube");
    std::map<std::array<std::uint8_t, 4>, int> colors;
    int covered = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        const auto pixel = pixels.at(x, y);
        if (opaque(pixel)) {
          ++colors[pixel];
          ++covered;
        } else {
          TEST_CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0);
        }
      }
    }
    TEST_CHECK(covered > kWidth * kHeight / 10);
    std::vector<std::array<std::uint8_t, 4>> faces;
    for (const auto& [color, count] : colors) {
      if (count > covered / 10) {
        faces.push_back(color);
      }
    }
    TEST_CHECK(faces.size() == 3);
    for (std::size_t a = 0; a < faces.size(); ++a) {
      for (std::size_t b = a + 1; b < faces.size(); ++b) {
        const int red = std::abs(faces[a][0] - faces[b][0]);
        const int blue = std::abs(faces[a][2] - faces[b][2]);
        TEST_CHECK(std::max(red, blue) > 30);
      }
    }
    TEST_CHECK(pixels.at(0, 0)[3] == 0 && pixels.at(kWidth - 1, kHeight - 1)[3] == 0);
  }

  // A small square in front of a large one hides it, whatever the order of their triangles.
  void nearerHidesFarther(MeshProgram& program) {
    for (const bool nearFirst : {true, false}) {
      Mesh mesh;
      if (nearFirst) {
        addSquare(mesh, 1, 0.5F, vec(kMeshKeyLight));
        addSquare(mesh, 2, -0.5F, vec(kMeshFillLight));
      } else {
        addSquare(mesh, 2, -0.5F, vec(kMeshFillLight));
        addSquare(mesh, 1, 0.5F, vec(kMeshKeyLight));
      }
      const MeshGeometry squares = geometryOf(std::move(mesh));
      const GlPixels pixels =
          renderImage(program, squares, lookAt(squares, MeshOrbit{.yaw = 0, .pitch = 0, .zoom = 1}));
      if (nearFirst) {
        save(pixels, "depth");
      }
      const auto center = pixels.at(kWidth / 2, kHeight / 2);
      TEST_CHECK(center[0] > 200 && center[2] < 40);
      // Straight below the center, the first pixel past the near square shows the far square's own color.
      int y = kHeight / 2;
      while (y < kHeight - 1 && pixels.at(kWidth / 2, y)[0] > 200) {
        ++y;
      }
      TEST_CHECK(y < kHeight * 9 / 10);
      const auto rim = pixels.at(kWidth / 2, y + 2);
      TEST_CHECK(opaque(rim) && rim[2] > 100 && rim[0] < 120);
    }
  }

  // Filled pixels in the upper and the lower half.
  std::pair<int, int> halves(const GlPixels& pixels) {
    int upper = 0;
    int lower = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        if (opaque(pixels.at(x, y))) {
          (y < pixels.height / 2 ? upper : lower) += 1;
        }
      }
    }
    return {upper, lower};
  }

  void imageIsTopRowFirst(MeshProgram& program) {
    const MeshGeometry triangle = lowTriangle();
    auto gpu = program.createGpu();
    TEST_CHECK(program.render(*gpu, triangle, 1, lookAt(triangle, MeshOrbit{0, 0, 1}, 200, 100), 200, 100));
    TEST_CHECK(gpu->image().valid() && gpu->imageWidth() == 200 && gpu->imageHeight() == 100);
    // glReadPixels gives the bottom GL row first, which is the image's top row.
    const auto [upper, lower] = halves(readImage(*gpu));
    TEST_CHECK(lower > upper * 2 && upper > 0);
    gpu->release();
  }

  void rendersOnlyWhenSomethingChanged(MeshProgram& program) {
    const MeshGeometry box = cube();
    auto gpu = program.createGpu();
    MeshLook look = lookAt(box, MeshOrbit{}, 64, 48);
    TEST_CHECK(program.render(*gpu, box, 1, look, 64, 48));
    TEST_CHECK(program.render(*gpu, box, 1, look, 64, 48));
    TEST_CHECK(gpu->renders() == 1 && gpu->uploads() == 1);
    look.camera = lookAt(box, MeshOrbit{.yaw = 1}, 64, 48).camera;
    TEST_CHECK(program.render(*gpu, box, 1, look, 64, 48));
    TEST_CHECK(gpu->renders() == 2);
    TEST_CHECK(program.render(*gpu, box, 1, look, 80, 48));
    TEST_CHECK(gpu->renders() == 3 && gpu->imageWidth() == 80);
    look.fillLight = kRed;
    TEST_CHECK(program.render(*gpu, box, 1, look, 80, 48));
    TEST_CHECK(gpu->renders() == 4 && gpu->uploads() == 1);
    TEST_CHECK(program.render(*gpu, box, 2, look, 80, 48));
    TEST_CHECK(gpu->renders() == 5 && gpu->uploads() == 2);
    TEST_CHECK(gpu->bytes() >= (36 * 16) + (36 * 4) + (80 * 48 * 6));
    // Released objects come back on the next render.
    gpu->release();
    TEST_CHECK(gpu->bytes() == 0 && !gpu->image().valid());
    TEST_CHECK(program.render(*gpu, box, 2, look, 80, 48));
    TEST_CHECK(gpu->renders() == 6 && gpu->uploads() == 3 && gpu->image().valid());
    gpu->release();
    // Sizes past the driver's and the program's limit are clamped; an empty size draws nothing.
    TEST_CHECK(program.render(*gpu, box, 2, look, MeshProgram::kMaxImageSide * 2, 8));
    TEST_CHECK(gpu->imageWidth() <= MeshProgram::kMaxImageSide && gpu->imageHeight() == 8);
    TEST_CHECK(!program.render(*gpu, box, 2, look, 0, 48));
    gpu->release();
  }

  void droppedGpuIsFreedOnNextRender(MeshProgram& program) {
    const MeshGeometry box = cube();
    std::weak_ptr<MeshGpu> watched;
    {
      auto gpu = program.createGpu();
      watched = gpu;
      TEST_CHECK(program.render(*gpu, box, 1, lookAt(box, MeshOrbit{}, 32, 32), 32, 32));
    }
    TEST_CHECK(!watched.expired());
    (void)renderImage(program, box, lookAt(box, MeshOrbit{}));
    TEST_CHECK(watched.expired());
  }

  // Leaves a state unlike the defaults in place, so a draw that resets instead of restoring is caught.
  void setUnusualState() {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    glScissor(3, 4, 200, 150);
    glViewport(1, 2, 300, 220);
    glClearColor(0.25F, 0.5F, 0.75F, 1.0F);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_FALSE);
    static const std::array<float, 8> kFloats{};
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (GLuint index = 0; index < 2; ++index) {
      glVertexAttribPointer(index, 2, GL_FLOAT, GL_FALSE, 8, kFloats.data());
      glEnableVertexAttribArray(index);
    }
  }

  struct ExtraState {
    GLint framebuffer = 0;
    GLint elementBuffer = 0;
    GLint renderbuffer = 0;
    GLint depthFunc = 0;
    GLboolean depthMask = GL_TRUE;
    std::array<GLfloat, 4> clearColor{};
  };

  ExtraState captureExtra() {
    ExtraState s;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elementBuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s.renderbuffer);
    glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s.clearColor.data());
    return s;
  }

  void checkSame(const test::GlState& a, const test::GlState& b, const ExtraState& c, const ExtraState& d) {
    TEST_CHECK(a.program == b.program);
    TEST_CHECK(a.arrayBuffer == b.arrayBuffer);
    TEST_CHECK(a.activeTexture == b.activeTexture);
    TEST_CHECK(a.texture == b.texture);
    TEST_CHECK(a.blend == b.blend);
    TEST_CHECK(a.scissor == b.scissor);
    TEST_CHECK(a.depthTest == b.depthTest);
    TEST_CHECK(a.blendFunc == b.blendFunc);
    TEST_CHECK(a.scissorBox == b.scissorBox);
    TEST_CHECK(a.viewport == b.viewport);
    TEST_CHECK(a.attributes.size() == b.attributes.size());
    for (std::size_t i = 0; i < a.attributes.size(); ++i) {
      const auto& x = a.attributes[i];
      const auto& y = b.attributes[i];
      TEST_CHECK(x.enabled == y.enabled && x.size == y.size && x.type == y.type);
      TEST_CHECK(x.normalized == y.normalized && x.stride == y.stride);
      TEST_CHECK(x.buffer == y.buffer && x.pointer == y.pointer);
    }
    TEST_CHECK(c.framebuffer == d.framebuffer);
    TEST_CHECK(c.elementBuffer == d.elementBuffer);
    TEST_CHECK(c.renderbuffer == d.renderbuffer);
    TEST_CHECK(c.depthFunc == d.depthFunc);
    TEST_CHECK(c.depthMask == d.depthMask);
    TEST_CHECK(c.clearColor == d.clearColor);
  }

  void glStateRestored(MeshProgram& program) {
    const MeshGeometry box = cube();
    setUnusualState();
    auto gpu = program.createGpu();
    const auto before = test::captureGlState();
    const auto extraBefore = captureExtra();
    TEST_CHECK(program.render(*gpu, box, 1, lookAt(box, MeshOrbit{}, 100, 80), 100, 80));
    checkSame(before, test::captureGlState(), extraBefore, captureExtra());
    gpu->release();
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, kWidth, kHeight);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
  }

  // One million flat triangles in a grid, each with its own corners, as an STL file gives them.
  MeshGeometry millionTriangles() {
    MeshGeometry geometry;
    geometry.flatNormals = true;
    geometry.vertices.resize(3'000'000);
    geometry.indices.resize(3'000'000);
    for (std::uint32_t t = 0; t < 1'000'000; ++t) {
      const auto x = static_cast<float>((t / 2) % 1000);
      const auto y = static_cast<float>((t / 2) / 1000);
      const float z = std::sin(x * 0.05F) * std::cos(y * 0.05F) * 10.0F;
      const bool upper = t % 2 == 1;
      const std::array<std::array<float, 3>, 3> corners{{
          {x, y, z},
          {upper ? x : x + 1, upper ? y + 1 : y, z},
          {x + 1, y + 1, z},
      }};
      for (std::uint32_t c = 0; c < 3; ++c) {
        geometry.vertices[(t * 3) + c] = {.position = corners.at(c), .normal = {0, 0, 127, 0}};
        geometry.indices[(t * 3) + c] = (t * 3) + c;
      }
    }
    geometry.bounds = {.min = Vec3{0, 0, -10}, .max = Vec3{1000, 500, 10}};
    return geometry;
  }

  void measureMillionTriangles(MeshProgram& program) {
    const MeshGeometry geometry = millionTriangles();
    auto gpu = program.createGpu();
    const MeshLook look = lookAt(geometry, MeshOrbit{}, 1024, 768);
    const auto wallStart = std::chrono::steady_clock::now();
    const std::clock_t start = std::clock();
    TEST_CHECK(program.render(*gpu, geometry, 1, look, 1024, 768));
    glFinish();
    const double first = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    const double firstWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    MeshLook turned = look;
    turned.camera = lookAt(geometry, MeshOrbit{.yaw = 1}, 1024, 768).camera;
    const auto wallAgain = std::chrono::steady_clock::now();
    const std::clock_t again = std::clock();
    TEST_CHECK(program.render(*gpu, geometry, 1, turned, 1024, 768));
    glFinish();
    const double second = static_cast<double>(std::clock() - again) / CLOCKS_PER_SEC;
    const double secondWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallAgain).count();
    TEST_CHECK(gpu->uploads() == 1 && gpu->renders() == 2);
    TEST_CHECK(first < 10.0);
    std::println(
        "mesh program: 1000000 triangles, upload and first draw {:.1f} ms ({:.1f} ms processor), redraw {:.1f} ms "
        "({:.1f} ms processor) at 1024x768, {:.1f} MiB on the GPU, {} indices, on {}",
        firstWall * 1000, first * 1000, secondWall * 1000, second * 1000,
        static_cast<double>(gpu->bytes()) / (1024.0 * 1024.0), program.uintIndices() ? "32-bit" : "16-bit",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER))
    );
    gpu->release();
  }
} // namespace

int main() {
  std::string reason;
  const auto gl = test::GlTestContext::create(kWidth, kHeight, true, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  {
    MeshProgram program;
    program.ensureInitialized();
    cubeFacesDiffer(program);
    nearerHidesFarther(program);
    imageIsTopRowFirst(program);
    rendersOnlyWhenSomethingChanged(program);
    droppedGpuIsFreedOnNextRender(program);
    glStateRestored(program);
    measureMillionTriangles(program);
    program.destroy();
  }
  std::println("mesh program checks passed");
}
