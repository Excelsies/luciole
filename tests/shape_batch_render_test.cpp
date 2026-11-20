#include "gl_test_context.h"
#include "render/backend/render_backend.h"
#include "render/core/shader_program.h"
#include "test_check.h"
#include "ui/controls/shape_batch_node.h"
#include "ui/controls/shape_batch_program.h"
#include "workbench/shape.h"
#include "workbench/shape_mesh.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace {
  using namespace luciole::workbench;
  using test::GlPixels;

  constexpr Rgba8 kWhite{255, 255, 255, 255};
  constexpr Rgba8 kRed{255, 0, 0, 255};
  constexpr Rgba8 kBlue{0, 0, 255, 255};
  constexpr std::array<float, 3> kScales{1.0F, 1.25F, 2.0F};
  constexpr int kRegion = 200; // device pixels the checks look at

  struct Fixture {
    test::GlTestContext& gl;
    RenderBackend& backend;
  };

  struct Pass {
    float scale = 1;
    float opacity = 1;
    Mat3 transform = Mat3::identity();
    std::array<float, 4> clear{};
  };

  GlPixels render(const Fixture& f, std::span<const Shape> shapes, const Pass& pass) {
    ShapeBatchNode node;
    node.setShapes(shapes, pass.scale);
    f.gl.clear(pass.clear[0], pass.clear[1], pass.clear[2], pass.clear[3]);
    f.backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
    node.draw(
        f.backend,
        CustomDrawContext{
            .surfaceWidth = static_cast<float>(f.gl.width()) / pass.scale,
            .surfaceHeight = static_cast<float>(f.gl.height()) / pass.scale,
            .pixelScaleX = pass.scale,
            .pixelScaleY = pass.scale,
            .transform = pass.transform,
            .opacity = pass.opacity,
        }
    );
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    return f.gl.readPixels();
  }

  GlPixels render(const Fixture& f, const Shape& shape, const Pass& pass) {
    return render(f, std::span<const Shape>(&shape, 1), pass);
  }

  Shape filled(Shape shape, Rgba8 fill) {
    shape.fill = fill;
    return shape;
  }

  ShapePoint center(int px, int py, float scale) {
    return {(static_cast<float>(px) + 0.5F) / scale, (static_cast<float>(py) + 0.5F) / scale};
  }

  // Whether each channel is within `tolerance` of `expected`; prints both when not.
  bool near(std::array<std::uint8_t, 4> pixel, std::array<int, 4> expected, int tolerance) {
    for (std::size_t i = 0; i < 4; ++i) {
      if (std::abs(static_cast<int>(pixel[i]) - expected[i]) > tolerance) {
        std::println(
            stderr, "pixel {} {} {} {}, expected {} {} {} {}", pixel[0], pixel[1], pixel[2], pixel[3], expected[0],
            expected[1], expected[2], expected[3]
        );
        return false;
      }
    }
    return true;
  }

  void fillAndOutside(const Fixture& f) {
    const auto pixels = render(f, filled(rectShape({1, 0}, 10, 10, 40, 20), kRed), {});
    TEST_CHECK(near(pixels.at(30, 20), {255, 0, 0, 255}, 1));
    TEST_CHECK(near(pixels.at(11, 11), {255, 0, 0, 255}, 1));
    TEST_CHECK((pixels.at(30, 40) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    TEST_CHECK((pixels.at(60, 20) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    TEST_CHECK((pixels.at(8, 20) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
  }

  // Coverage follows the distance to the edge over one device pixel, so edge pixels are partly covered and a
  // curve's coverage changes smoothly instead of in steps.
  void antialiasedEdges(const Fixture& f) {
    for (const float scale : kScales) {
      const Point c{40.3, 39.6};
      const float r = 30.4F;
      const auto pixels = render(f, filled(circleShape({1, 0}, c, r), kWhite), {.scale = scale});
      int partial = 0;
      for (int py = 0; py < kRegion; ++py) {
        for (int px = 0; px < kRegion; ++px) {
          const auto p = center(px, py, scale);
          const float d = std::hypot(p.x - static_cast<float>(c.x), p.y - static_cast<float>(c.y)) - r;
          const float expected = std::clamp(0.5F - d * scale, 0.0F, 1.0F) * 255;
          const int alpha = pixels.at(px, py)[3];
          TEST_NEAR(static_cast<float>(alpha), expected, 6.0F);
          partial += alpha > 8 && alpha < 247 ? 1 : 0;
        }
      }
      // Most pixels the circle's edge crosses are partly covered.
      TEST_CHECK(static_cast<float>(partial) > 2 * std::numbers::pi_v<float> * r * scale * 0.6F);
      const auto edge = render(f, filled(rectShape({1, 0}, 10.5, 10, 40, 30), kWhite), {.scale = scale});
      const int column = static_cast<int>(std::floor(10.5F * scale));
      const float covered = std::ceil(10.5F * scale) - 10.5F * scale;
      for (int py = static_cast<int>(12 * scale); py < static_cast<int>(38 * scale); ++py) {
        TEST_NEAR(static_cast<float>(edge.at(column, py)[3]), (covered > 0 ? covered : 1.0F) * 255, 6.0F);
      }
    }
  }

  void strokeOnEdge(const Fixture& f) {
    for (const float scale : kScales) {
      auto box = filled(rectShape({1, 0}, 20, 20, 60, 40), kBlue);
      box.stroke = kWhite;
      box.strokeWidth = 3;
      const auto pixels = render(f, box, {.scale = scale});
      const int x = static_cast<int>(50 * scale);
      TEST_CHECK(near(pixels.at(x, static_cast<int>(21.5F * scale)), {255, 255, 255, 255}, 2));
      TEST_CHECK(near(pixels.at(x, static_cast<int>(30 * scale)), {0, 0, 255, 255}, 2));
      TEST_CHECK(
          near(pixels.at(static_cast<int>(78.5F * scale), static_cast<int>(40 * scale)), {255, 255, 255, 255}, 2)
      );
      TEST_CHECK(pixels.at(x, static_cast<int>(18 * scale))[3] == 0);
    }
    auto ring = filled(sectorShape({1, 0}, {50, 50}, 20, 45, 30, 120), kBlue);
    ring.stroke = kWhite;
    ring.strokeWidth = 2;
    const auto pixels = render(f, ring, {});
    const auto along = [](double radius) {
      const double middle = 75.0 * std::numbers::pi / 180.0;
      return std::array{
          static_cast<int>(50 + radius * std::sin(middle)), static_cast<int>(50 - radius * std::cos(middle))
      };
    };
    TEST_CHECK(near(pixels.at(along(44)[0], along(44)[1]), {255, 255, 255, 255}, 40));
    TEST_CHECK(near(pixels.at(along(32)[0], along(32)[1]), {0, 0, 255, 255}, 2));
  }

  // Coverage of every pixel agrees with the pure hit test, except within one device pixel of an edge.
  void matchesHitTest(const Fixture& f, const Shape& shape, float scale) {
    const auto pixels = render(f, filled(shape, kWhite), {.scale = scale});
    const float pixel = 1.0F / scale;
    int covered = 0;
    for (int py = 0; py < kRegion; ++py) {
      for (int px = 0; px < kRegion; ++px) {
        const auto p = center(px, py, scale);
        const bool drawn = pixels.at(px, py)[3] >= 128;
        covered += drawn ? 1 : 0;
        if (drawn == shapeContains(shape, p, 0)) {
          continue;
        }
        if (drawn) {
          TEST_CHECK(shapeContains(shape, p, pixel));
          continue;
        }
        const std::array<ShapePoint, 4> around{
            {{p.x - pixel, p.y}, {p.x + pixel, p.y}, {p.x, p.y - pixel}, {p.x, p.y + pixel}}
        };
        TEST_CHECK(std::ranges::any_of(around, [&](ShapePoint q) { return !shapeContains(shape, q, 0); }));
      }
    }
    TEST_CHECK(covered > 0);
  }

  Shape ribbonShape(std::array<ShapePoint, 4> top, std::array<ShapePoint, 4> bottom) {
    Shape shape;
    shape.id = {1, 0};
    shape.kind = ShapeKind::Ribbon;
    shape.ribbon = {.top = top, .bottom = bottom};
    return shape;
  }

  void shapesWhereHitTestSays(const Fixture& f) {
    const std::vector<Shape> shapes{
        sectorShape({1, 0}, {40, 40}, 12, 36, -30, 60),
        sectorShape({1, 0}, {40, 40}, 10, 38, 100, 330),
        sectorShape({1, 0}, {40, 40}, 0, 30, 45, 135),
        sectorShape({1, 0}, {40, 40}, 20, 34, 0, 360),
        sectorShape({1, 0}, {40, 40}, 5, 37, 350, 375),
        rectShape({1, 0}, 8.3, 12.6, 61.2, 40.1, 9),
        circleShape({1, 0}, {41.2, 38.7}, 26.3),
        lineShape({1, 0}, {10, 12}, {70, 61}, 3),
        ribbonShape({{{8, 10}, {40, 10}, {40, 40}, {75, 40}}}, {{{8, 30}, {40, 30}, {40, 72}, {75, 72}}}),
    };
    for (const float scale : kScales) {
      for (const auto& shape : shapes) {
        matchesHitTest(f, shape, scale);
      }
    }
  }

  void patternsShow(const Fixture& f) {
    auto hatched = filled(rectShape({1, 0}, 10, 10, 60, 60), kBlue);
    hatched.pattern = ShapePattern::Hatched;
    hatched.stroke = kWhite;
    const auto stripes = render(f, hatched, {});
    int stripePixels = 0;
    for (int py = 12; py < 68; ++py) {
      for (int px = 12; px < 68; ++px) {
        // A stripe's middle lies 0.75 past each period: the pixel at 1 is covered 0.854, those at 3 and 4 not.
        const int phase = (px + py + 1) % 6;
        if (phase == 1) {
          ++stripePixels;
          TEST_CHECK(near(stripes.at(px, py), {218, 218, 255, 255}, 4));
        } else if (phase == 3 || phase == 4) {
          TEST_CHECK(near(stripes.at(px, py), {0, 0, 255, 255}, 2));
        }
      }
    }
    TEST_CHECK(stripePixels > 200);
    auto dashed = filled(rectShape({1, 0}, 10, 10, 70, 30), kBlue);
    dashed.pattern = ShapePattern::DashedOutline;
    dashed.stroke = kWhite;
    dashed.strokeWidth = 2;
    const auto dashes = render(f, dashed, {});
    for (int px = 15; px < 75; ++px) {
      const float local = static_cast<float>(px) + 0.5F - 45;
      const bool dash = std::fmod(std::fmod(local, 7.0F) + 7.0F, 7.0F) < 4;
      TEST_CHECK(near(dashes.at(px, 10), dash ? std::array{255, 255, 255, 255} : std::array{0, 0, 255, 255}, 2));
    }
  }

  void premultipliedOpacity(const Fixture& f) {
    auto white = filled(rectShape({1, 0}, 10, 10, 40, 40), kWhite);
    white.opacity = 0.5F;
    TEST_CHECK(near(render(f, white, {}).at(30, 30), {128, 128, 128, 128}, 2));
    const auto translucent = filled(rectShape({1, 0}, 10, 10, 40, 40), {255, 0, 0, 128});
    TEST_CHECK(near(render(f, translucent, {}).at(30, 30), {128, 0, 0, 128}, 2));
    auto red = filled(rectShape({1, 0}, 10, 10, 40, 40), kRed);
    red.opacity = 0.5F;
    const Pass overBlue{.clear = {0, 0, 1, 1}};
    TEST_CHECK(near(render(f, red, overBlue).at(30, 30), {128, 0, 127, 255}, 2));
    const Pass faded{.opacity = 0.5F, .clear = {0, 0, 1, 1}};
    TEST_CHECK(
        near(render(f, filled(rectShape({1, 0}, 10, 10, 40, 40), kRed), faded).at(30, 30), {128, 0, 127, 255}, 2)
    );
  }

  void ribbonCoversBand(const Fixture& f) {
    const auto band =
        ribbonShape({{{10, 20}, {40, 20}, {60, 20}, {90, 20}}}, {{{10, 50}, {40, 50}, {60, 50}, {90, 50}}});
    const auto pixels = render(f, filled(band, kRed), {});
    for (int px = 12; px < 88; px += 4) {
      TEST_CHECK(near(pixels.at(px, 21), {255, 0, 0, 255}, 1));
      TEST_CHECK(near(pixels.at(px, 35), {255, 0, 0, 255}, 1));
      TEST_CHECK(near(pixels.at(px, 48), {255, 0, 0, 255}, 1));
      TEST_CHECK(pixels.at(px, 18)[3] == 0);
      TEST_CHECK(pixels.at(px, 52)[3] == 0);
    }
    TEST_CHECK(pixels.at(5, 35)[3] == 0);
    TEST_CHECK(pixels.at(95, 35)[3] == 0);
  }

  void transformMoves(const Fixture& f) {
    const auto pixels =
        render(f, filled(rectShape({1, 0}, 10, 10, 20, 20), kRed), {.transform = Mat3::translation(30, 0)});
    TEST_CHECK(near(pixels.at(45, 20), {255, 0, 0, 255}, 1));
    TEST_CHECK(pixels.at(15, 20)[3] == 0);
  }

  void sameState(const test::GlState& before, const test::GlState& after) {
    TEST_CHECK(after.program == before.program);
    TEST_CHECK(after.arrayBuffer == before.arrayBuffer);
    TEST_CHECK(after.activeTexture == before.activeTexture);
    TEST_CHECK(after.texture == before.texture);
    TEST_CHECK(after.blend == before.blend);
    TEST_CHECK(after.scissor == before.scissor);
    TEST_CHECK(after.depthTest == before.depthTest);
    TEST_CHECK(after.viewport == before.viewport);
    TEST_CHECK(after.blendFunc == before.blendFunc);
    TEST_CHECK(after.scissorBox == before.scissorBox);
    TEST_CHECK(after.attributes.size() == before.attributes.size());
    for (std::size_t i = 0; i < before.attributes.size(); ++i) {
      const auto& a = after.attributes[i];
      const auto& b = before.attributes[i];
      TEST_CHECK(a.enabled == b.enabled);
      TEST_CHECK(a.size == b.size);
      TEST_CHECK(a.type == b.type);
      TEST_CHECK(a.normalized == b.normalized);
      TEST_CHECK(a.stride == b.stride);
      TEST_CHECK(a.buffer == b.buffer);
      TEST_CHECK(a.pointer == b.pointer);
    }
  }

  void drawNode(const Fixture& f, const ShapeBatchNode& node) {
    node.draw(
        f.backend,
        CustomDrawContext{
            .surfaceWidth = static_cast<float>(f.gl.width()),
            .surfaceHeight = static_cast<float>(f.gl.height()),
        }
    );
  }

  // Another program, client and buffer attribute arrays, a second texture unit and a scissor are in place, as other
  // drawing of the frame leaves them; the draw changes none of it.
  void glStateRestored(const Fixture& f) {
    ShaderProgram other;
    other.create(
        "attribute vec4 a_point; void main() { gl_Position = a_point; }",
        "precision mediump float; void main() { gl_FragColor = vec4(1.0); }"
    );
    static const std::array<float, 8> client{};
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, 64, nullptr, GL_STATIC_DRAW);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 12, reinterpret_cast<const void*>(16));
    glEnableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, client.data());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, 4, client.data());
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glUseProgram(other.id());
    glActiveTexture(GL_TEXTURE1);
    f.backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
    f.backend.setScissor({.x = 0, .y = 100, .width = 600, .height = 700});
    f.gl.clear(0, 0, 0, 0);

    std::vector<Shape> shapes{
        filled(rectShape({1, 0}, 10, 10, 40, 40), kRed),
        filled(sectorShape({2, 0}, {120, 60}, 10, 40, 10, 200), kBlue),
        filled(
            ribbonShape({{{150, 10}, {170, 10}, {180, 30}, {200, 30}}}, {{{150, 20}, {170, 20}, {180, 60}, {200, 60}}}),
            kRed
        ),
    };
    ShapeBatchNode node;
    node.setShapes(shapes, 1);
    const auto before = test::captureGlState();
    drawNode(f, node);
    const auto after = test::captureGlState();
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    sameState(before, after);
    TEST_CHECK(near(f.gl.readPixels().at(30, 30), {255, 0, 0, 255}, 1));

    f.backend.disableScissor();
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &buffer);
  }

  // A node uploads on its first draw after a change. A program that released or lost its objects builds them again
  // and the node uploads again.
  void uploadsOnChange(const Fixture& f) {
    auto& program = f.backend.extension<ShapeBatchProgram>();
    const auto drawn = [&](const ShapeBatchNode& node) {
      f.gl.clear(0, 0, 0, 0);
      drawNode(f, node);
      return f.gl.readPixels().at(30, 30);
    };
    const auto red = filled(rectShape({1, 0}, 10, 10, 40, 40), kRed);
    const auto blue = filled(red, kBlue);
    {
      ShapeBatchNode dropped;
      dropped.setShapes(std::span(&red, 1), 1);
      drawn(dropped);
    }
    ShapeBatchNode node;
    node.setShapes(std::span(&red, 1), 1);
    const auto start = program.uploads();
    TEST_CHECK(near(drawn(node), {255, 0, 0, 255}, 1));
    TEST_CHECK(near(drawn(node), {255, 0, 0, 255}, 1));
    TEST_CHECK(program.uploads() == start + 1);
    node.setShapes(std::span(&blue, 1), 1);
    TEST_CHECK(near(drawn(node), {0, 0, 255, 255}, 1));
    TEST_CHECK(program.uploads() == start + 2);
    program.destroy();
    TEST_CHECK(near(drawn(node), {0, 0, 255, 255}, 1));
    TEST_CHECK(program.uploads() == start + 3);
    program.abandon();
    TEST_CHECK(near(drawn(node), {0, 0, 255, 255}, 1));
    TEST_CHECK(program.uploads() == start + 4);
    node.setShapes({}, 1);
    TEST_CHECK(drawn(node)[3] == 0);
    TEST_CHECK(program.uploads() == start + 4);
    TEST_CHECK(glGetError() == GL_NO_ERROR);
  }

  // 4,000 tiles, 940 ring sectors and 60 ribbons over a 1280 by 800 surface.
  std::vector<Shape> manyShapes() {
    std::vector<Shape> shapes;
    shapes.reserve(5000);
    for (int i = 0; i < 4000; ++i) {
      auto tile = filled(rectShape({static_cast<NodeId>(i), 0}, (i % 80) * 16.0, (i / 80) * 16.0, 15, 15, 3), kBlue);
      tile.stroke = kWhite;
      tile.strokeWidth = 1;
      shapes.push_back(tile);
    }
    for (int i = 0; i < 940; ++i) {
      const double ring = 40.0 + (i / 100) * 30.0;
      const double a0 = (i % 100) * 3.6;
      shapes.push_back(
          filled(sectorShape({static_cast<NodeId>(4000 + i), 0}, {640, 400}, ring, ring + 28, a0, a0 + 3.4), kRed)
      );
    }
    for (int i = 0; i < 60; ++i) {
      const auto y = static_cast<float>(i * 12);
      shapes.push_back(filled(
          ribbonShape(
              {{{100, y}, {180, y}, {180, y + 60}, {260, y + 60}}},
              {{{100, y + 5}, {180, y + 5}, {180, y + 66}, {260, y + 66}}}
          ),
          kWhite
      ));
    }
    return shapes;
  }

  double medianMs(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
  }

  void measureFiveThousand(const Fixture& f) {
    using Clock = std::chrono::steady_clock;
    const auto shapes = manyShapes();
    const auto ms = [](Clock::time_point start) {
      return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    ShapeMesh mesh;
    std::vector<double> build;
    std::vector<double> uploadAndDraw;
    std::vector<double> drawOnly;
    ShapeBatchNode node;
    for (int run = 0; run < 7; ++run) {
      auto start = Clock::now();
      buildShapeMesh(shapes, 1, mesh);
      build.push_back(ms(start));
      node.setShapes(shapes, 1);
      glFinish();
      start = Clock::now();
      drawNode(f, node);
      glFinish();
      uploadAndDraw.push_back(ms(start));
      start = Clock::now();
      drawNode(f, node);
      glFinish();
      drawOnly.push_back(ms(start));
    }
    TEST_CHECK(mesh.drawn == 5000);
    TEST_CHECK(!mesh.truncated);
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    std::println(
        "5000 shapes, {} vertices on {}: mesh build {:.2f} ms, upload and draw {:.2f} ms, draw {:.2f} ms (medians of "
        "7)",
        mesh.vertices.size(), reinterpret_cast<const char*>(glGetString(GL_RENDERER)), medianMs(build),
        medianMs(uploadAndDraw), medianMs(drawOnly)
    );
  }
} // namespace

int main() {
  std::string reason;
  const auto gl = test::GlTestContext::create(1280, 800, false, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  {
    const auto backend = createDefaultRenderBackend();
    const Fixture f{*gl, *backend};
    fillAndOutside(f);
    antialiasedEdges(f);
    strokeOnEdge(f);
    shapesWhereHitTestSays(f);
    patternsShow(f);
    premultipliedOpacity(f);
    ribbonCoversBand(f);
    transformMoves(f);
    glStateRestored(f);
    uploadsOnChange(f);
    measureFiveThousand(f);
  }
  std::println("shape batch render checks passed");
}
