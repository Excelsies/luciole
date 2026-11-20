#include "gl_test_context.h"
#include "png_writer.h"
#include "render/backend/render_backend.h"
#include "test_check.h"
#include "ui/controls/shape_batch_node.h"
#include "workbench/shape.h"
#include "workbench/shape_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

// The shape batch's hatching, hatch color and soft edges, read back pixel by pixel at several scales.
namespace {
  using namespace luciole::workbench;
  using test::GlPixels;

  constexpr int kSize = 400; // device pixels of the framebuffer, both ways
  constexpr Rgba8 kBlack{0, 0, 0, 255};
  constexpr Rgba8 kWhite{255, 255, 255, 255};
  constexpr Rgba8 kBlue{0, 0, 255, 255};
  constexpr Rgba8 kYellow{255, 255, 0, 255};
  constexpr Rgba8 kGray{128, 128, 128, 255};
  constexpr std::array<float, 3> kScales{1.0F, 1.5F, 2.0F};

  struct Fixture {
    test::GlTestContext& gl;
    RenderBackend& backend;
  };

  GlPixels render(const Fixture& f, const Shape& shape, float scale, Mat3 transform = Mat3::identity()) {
    ShapeBatchNode node;
    node.setShapes(std::span(&shape, 1), scale);
    f.gl.clear(0, 0, 0, 0);
    f.backend.setBlendMode(RenderBlendMode::PremultipliedAlpha);
    node.draw(
        f.backend,
        CustomDrawContext{
            .surfaceWidth = static_cast<float>(kSize) / scale,
            .surfaceHeight = static_cast<float>(kSize) / scale,
            .pixelScaleX = scale,
            .pixelScaleY = scale,
            .transform = transform,
        }
    );
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    return f.gl.readPixels();
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

  // A run of pixels along a scan line where a stripe shows: its first pixel, width, summed coverage and the
  // coverage-weighted middle, in device pixels.
  struct Run {
    int first = 0;
    int width = 0;
    float sum = 0;
    float middle = 0;
    bool partial = false; // holds a pixel neither empty nor full
  };

  // Runs on row `py` between columns x0 and x1, cut off runs at either end left out. Coverage is the red channel
  // of white stripes over black.
  std::vector<Run> stripeRuns(const GlPixels& pixels, int py, int x0, int x1) {
    std::vector<Run> runs;
    Run run;
    bool open = false;
    for (int px = x0; px < x1; ++px) {
      const float v = static_cast<float>(pixels.at(px, py)[0]) / 255.0F;
      if (v > 0.02F) {
        if (!open) {
          run = {.first = px};
          open = true;
        }
        ++run.width;
        run.sum += v;
        run.middle += v * (static_cast<float>(px) + 0.5F);
        run.partial = run.partial || (v > 0.04F && v < 0.96F);
      } else if (open) {
        run.middle /= run.sum;
        if (run.first > x0) {
          runs.push_back(run);
        }
        open = false;
      }
    }
    return runs;
  }

  // Stripes fade over about one device pixel at every scale and position: each run of a scan line covers the
  // stripe's width once, holds a partly covered pixel, sits one period from the next and keeps its phase on the
  // part's own coordinates. A stray wider or brighter segment changes a run's sum.
  void hatchingIsSmooth(const Fixture& f) {
    constexpr std::array<std::array<float, 2>, 2> kOffsets{{{0, 0}, {0.37F, 0.61F}}};
    auto shape = rectShape({1, 0}, 10, 10, 170, 170);
    shape.fill = kBlack;
    shape.stroke = kWhite;
    shape.pattern = ShapePattern::Hatched;
    for (const float scale : kScales) {
      for (const auto& [tx, ty] : kOffsets) {
        const auto pixels = render(f, shape, scale, Mat3::translation(tx, ty));
        if (tx == 0) {
          keep(pixels, std::format("hatch-{}x.png", scale));
        }
        const int x0 = static_cast<int>(std::ceil((14 + tx) * scale));
        const int x1 = static_cast<int>((176 + tx) * scale);
        const float width = kHatchWidth * scale;
        std::size_t rows = 0;
        for (int py = static_cast<int>((14 + ty) * scale); py < static_cast<int>((176 + ty) * scale); py += 5) {
          const auto runs = stripeRuns(pixels, py, x0, x1);
          TEST_CHECK(static_cast<float>(runs.size()) >= static_cast<float>(x1 - x0) / (kHatchPeriod * scale) - 2);
          for (std::size_t i = 0; i < runs.size(); ++i) {
            const auto& run = runs[i];
            TEST_NEAR(run.sum, width, 0.25F);
            TEST_CHECK(run.partial);
            TEST_CHECK(run.width >= 2 && static_cast<float>(run.width) <= std::ceil(width + 1.5F) + 1);
            if (i > 0) {
              TEST_NEAR(run.middle - runs[i - 1].middle, kHatchPeriod * scale, 0.3F);
            }
            // The stripe's middle lies where x + y is half a stripe past a whole number of periods.
            const float x = run.middle / scale - tx;
            const float y = (static_cast<float>(py) + 0.5F) / scale - ty;
            const float phase = std::fmod(x + y - kHatchWidth / 2 + kHatchPeriod, kHatchPeriod);
            TEST_CHECK(std::min(phase, kHatchPeriod - phase) < 0.2F);
          }
          ++rows;
        }
        TEST_CHECK(rows > 20);
      }
    }
  }

  // Under an outline the stripes keep the hatch color; without one set they take the stroke color.
  void hatchColorUnderAnOutline(const Fixture& f) {
    auto shape = rectShape({1, 0}, 20, 20, 120, 80, 4);
    shape.fill = kBlue;
    shape.stroke = kYellow;
    shape.strokeWidth = 2;
    shape.pattern = ShapePattern::Hatched;
    shape.hatch = kGray;
    for (const float scale : {1.0F, 2.0F}) {
      const auto pixels = render(f, shape, scale);
      keep(pixels, std::format("hatch-color-{}x.png", scale));
      int gray = 0;
      for (int py = static_cast<int>(24 * scale); py < static_cast<int>(96 * scale); ++py) {
        for (int px = static_cast<int>(24 * scale); px < static_cast<int>(136 * scale); ++px) {
          const auto p = pixels.at(px, py);
          TEST_CHECK(std::abs(p[0] - p[1]) <= 2 && p[0] <= 130); // gray over blue, never yellow
          gray += p[0] > 100 ? 1 : 0;
        }
      }
      TEST_CHECK(gray > 400);
      TEST_CHECK(pixels.at(static_cast<int>(80 * scale), static_cast<int>(21 * scale))[2] < 10); // the outline
    }
    shape.hatch.reset();
    const auto pixels = render(f, shape, 1);
    int yellow = 0;
    for (int py = 24; py < 96; ++py) {
      for (int px = 24; px < 136; ++px) {
        const auto p = pixels.at(px, py);
        yellow += p[0] > 200 && p[1] > 200 && p[2] < 60 ? 1 : 0;
      }
    }
    TEST_CHECK(yellow > 400);
  }

  // Alpha of the pixel holding the logical point (x, y).
  int alphaNear(const GlPixels& pixels, float x, float y, float scale) {
    return pixels.at(static_cast<int>(std::floor(x * scale)), static_cast<int>(std::floor(y * scale)))[3];
  }

  // A soft rectangle fades across its edge over the softness each way, as a smooth step: half at the edge, full
  // and none past it, never rising outward, sides alike. Values are within half a device pixel of the point named.
  void softEdgeFalls(const Fixture& f) {
    auto shape = rectShape({1, 0}, 40, 40, 100, 80, 10);
    shape.fill = kWhite;
    shape.softness = 12;
    shape.hittable = false;
    for (const float scale : {1.0F, 2.0F}) {
      const auto pixels = render(f, shape, scale);
      keep(pixels, std::format("soft-edge-{}x.png", scale));
      const int row = static_cast<int>(80 * scale);
      for (int px = static_cast<int>(16 * scale); px < static_cast<int>(64 * scale); ++px) {
        const float d = 40 - (static_cast<float>(px) + 0.5F) / scale; // outside the left edge
        const int alpha = pixels.at(px, row)[3];
        if (d <= -12) {
          TEST_CHECK(alpha >= 254);
        } else if (d >= 12) {
          TEST_CHECK(alpha == 0);
        }
        if (px > static_cast<int>(16 * scale)) {
          TEST_CHECK(alpha >= pixels.at(px - 1, row)[3]);
        }
      }
      TEST_NEAR(static_cast<float>(alphaNear(pixels, 40, 80, scale)), 128.0F, 14.0F);
      TEST_NEAR(static_cast<float>(alphaNear(pixels, 46, 80, scale)), 0.844F * 255, 14.0F);
      TEST_NEAR(static_cast<float>(alphaNear(pixels, 34, 80, scale)), 0.156F * 255, 14.0F);
      TEST_CHECK(alphaNear(pixels, 30.5F, 80, scale) > 0); // the quad reaches past the edge
      TEST_NEAR(
          static_cast<float>(alphaNear(pixels, 90, 34, scale)), static_cast<float>(alphaNear(pixels, 34, 80, scale)),
          6.0F
      );
    }
  }

  // A soft outline is a blurred ring: clear in the middle and far outside, brightest on its band.
  void softRing(const Fixture& f) {
    auto ring = rectShape({1, 0}, 100, 100, 160, 120, 8);
    ring.fill.a = 0;
    ring.stroke = kWhite;
    ring.strokeWidth = 3;
    ring.softness = 6;
    const auto pixels = render(f, ring, 1);
    keep(pixels, "soft-ring-1x.png");
    const int row = 160;
    TEST_CHECK(pixels.at(180, row)[3] == 0);
    TEST_CHECK(pixels.at(90, row)[3] == 0);
    int peak = 0;
    int peakAt = 0;
    for (int px = 88; px < 116; ++px) {
      const int alpha = pixels.at(px, row)[3];
      peakAt = alpha > peak ? px : peakAt;
      peak = std::max(peak, alpha);
    }
    TEST_CHECK(peak > 60 && peak < 200);
    TEST_CHECK(peakAt >= 98 && peakAt <= 104);
  }
} // namespace

int main() {
  std::string reason;
  const auto gl = test::GlTestContext::create(kSize, kSize, false, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  {
    const auto backend = createDefaultRenderBackend();
    const Fixture f{*gl, *backend};
    hatchingIsSmooth(f);
    hatchColorUnderAnOutline(f);
    softEdgeFalls(f);
    softRing(f);
  }
  std::println("shape batch pattern checks passed");
  return 0;
}
