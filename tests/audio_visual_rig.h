#pragma once

#include "audio_frame_sources.h"
#include "gl_test_context.h"
#include "png_writer.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/color.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// What every sound visual's GL tests share: a run of a visual from generated frames in an offscreen context, with
// the GL state and the pixels outside its rectangle checked after every frame, pixel statistics, and PNG captures in
// LUCIOLE_CAPTURE_DIR.
namespace test::audio {
  // A visual's box in logical pixels, the buffer scale, and the opacity the host passes.
  struct AudioVisualTarget {
    int width = 0;
    int height = 0;
    float scale = 1;
    float opacity = 1;
  };
  // The sizes every visual is captured at.
  inline constexpr std::array<AudioVisualTarget, 4> kCaptureTargets{{
      {.width = 450, .height = 600, .scale = 1},
      {.width = 450, .height = 600, .scale = 1.5F},
      {.width = 900, .height = 500, .scale = 1},
      {.width = 900, .height = 500, .scale = 1.5F},
  }};

  inline Color colorOf(luciole::workbench::Rgb rgb) {
    return rgba(static_cast<float>(rgb.r) / 255, static_cast<float>(rgb.g) / 255, static_cast<float>(rgb.b) / 255, 1);
  }

  // The default dark theme's roles: Primary, Secondary and Tertiary, Surface behind, OnSurface for text.
  inline AudioVisualPalette rigPalette() {
    const luciole::workbench::ColorPalette roles;
    return {
        .accents = {colorOf(roles.primary), colorOf(roles.secondary), colorOf(roles.tertiary)},
        .background = colorOf(roles.surface),
        .text = colorOf(roles.onSurface),
    };
  }

  using Pixel = std::array<std::uint8_t, 4>;

  // `color` as an opaque framebuffer stores it.
  inline Pixel pixelOf(const Color& color) {
    const auto byte = [](float c) { return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0F, 1.0F) * 255)); };
    return {byte(color.r), byte(color.g), byte(color.b), 255};
  }

  // Rec. 709 luma from 0 to 255.
  inline int luma(const Pixel& p) {
    return static_cast<int>(std::lround((0.2126 * p[0]) + (0.7152 * p[1]) + (0.0722 * p[2])));
  }

  // Pixels from left to right and top to bottom, the far edges excluded.
  struct Region {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };
  inline Region wholeRegion(const GlPixels& pixels) {
    return {.left = 0, .top = 0, .right = pixels.width, .bottom = pixels.height};
  }

  // How many pixels matched and the mean of their centers, top row first; 0 for both when none matched.
  struct PixelStats {
    int count = 0;
    double centerX = 0;
    double centerY = 0;
  };

  template <class Match> PixelStats matchingPixels(const GlPixels& pixels, Region region, Match match) {
    region = {
        .left = std::max(region.left, 0),
        .top = std::max(region.top, 0),
        .right = std::min(region.right, pixels.width),
        .bottom = std::min(region.bottom, pixels.height),
    };
    PixelStats stats;
    double sumX = 0;
    double sumY = 0;
    for (int y = region.top; y < region.bottom; ++y) {
      for (int x = region.left; x < region.right; ++x) {
        if (match(pixels.at(x, y))) {
          ++stats.count;
          sumX += x + 0.5;
          sumY += y + 0.5;
        }
      }
    }
    if (stats.count > 0) {
      stats.centerX = (sumX / stats.count) - 0.5;
      stats.centerY = (sumY / stats.count) - 0.5;
    }
    return stats;
  }

  // Pixels of `region` whose luma is at least `minLuma`.
  inline PixelStats brightPixels(const GlPixels& pixels, int minLuma, Region region) {
    return matchingPixels(pixels, region, [&](const Pixel& p) { return luma(p) >= minLuma; });
  }

  // Pixels of `region` whose red, green or blue differs from `color` by more than `tolerance`.
  inline PixelStats pixelsOtherThan(const GlPixels& pixels, const Pixel& color, int tolerance, Region region) {
    return matchingPixels(pixels, region, [&](const Pixel& p) {
      return std::abs(p[0] - color[0]) > tolerance
          || std::abs(p[1] - color[1]) > tolerance
          || std::abs(p[2] - color[2]) > tolerance;
    });
  }

  // Pixels whose red, green or blue differ by more than `tolerance` between two images of one size.
  inline int differingPixels(const GlPixels& a, const GlPixels& b, int tolerance) {
    TEST_CHECK(a.width == b.width && a.height == b.height && a.rgba.size() == b.rgba.size());
    int count = 0;
    for (int y = 0; y < a.height; ++y) {
      for (int x = 0; x < a.width; ++x) {
        const auto p = a.at(x, y);
        const auto q = b.at(x, y);
        count +=
            std::abs(p[0] - q[0]) > tolerance || std::abs(p[1] - q[1]) > tolerance || std::abs(p[2] - q[2]) > tolerance
            ? 1
            : 0;
      }
    }
    return count;
  }

  // A capture's file name, such as "bars-kicks-450x600-1.5x.png".
  inline std::string captureName(std::string_view visual, std::string_view scene, const AudioVisualTarget& target) {
    return std::format("{}-{}-{}x{}-{}x.png", visual, scene, target.width, target.height, target.scale);
  }

  // Writes `pixels` as `name` into LUCIOLE_CAPTURE_DIR when that is set.
  inline void keepForViewing(const GlPixels& pixels, const std::string& name) {
    const char* dir = std::getenv("LUCIOLE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
      return;
    }
    const auto path = std::filesystem::path(dir) / name;
    TEST_CHECK(writePng(pixels, path));
    std::println("wrote {}", path.string());
  }

  struct AudioVisualCapture {
    GlPixels pixels;        // the rectangle after the last frame, top row first; empty for a rectangle without area
    bool stateKept = true;  // every draw left the GL state as it found it
    bool insideOnly = true; // no frame changed a pixel outside the rectangle
    // Processor time of one advance and draw on this thread, the run's first frame left out when it has more.
    double meanCpuMilliseconds = 0;
    double maxCpuMilliseconds = 0;
  };

  // The shared GL state the contract of AudioVisual names, which a visual must leave as it found it.
  struct RigGlState {
    GlState gl;
    GLint elementBuffer = 0;
    GLint framebuffer = 0;
    std::array<GLint, 2> textures{}; // bound to units 0 and 1
    GLint unpackAlignment = 0;
    std::array<GLint, 2> blendEquation{};
    std::array<GLfloat, 4> blendColor{};
    std::array<GLboolean, 4> colorMask{};
    std::array<GLfloat, 4> clearColor{};
    std::array<GLfloat, 1> lineWidth{};
    GLboolean depthMask = GL_TRUE;
    GLboolean cullFace = GL_FALSE;
    GLboolean stencilTest = GL_FALSE;
  };

  inline RigGlState captureRigState() {
    RigGlState s{.gl = captureGlState()};
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elementBuffer);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, s.textures.data());
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.textures[1]);
    glActiveTexture(static_cast<GLenum>(s.gl.activeTexture));
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s.unpackAlignment);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, s.blendEquation.data());
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEquation[1]);
    glGetFloatv(GL_BLEND_COLOR, s.blendColor.data());
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask.data());
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s.clearColor.data());
    glGetFloatv(GL_LINE_WIDTH, s.lineWidth.data());
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.cullFace = glIsEnabled(GL_CULL_FACE);
    s.stencilTest = glIsEnabled(GL_STENCIL_TEST);
    return s;
  }

  // The names of the pieces of state that differ.
  inline std::vector<std::string> changedState(const RigGlState& a, const RigGlState& b) {
    std::vector<std::string> changed;
    const auto note = [&](bool same, std::string name) {
      if (!same) {
        changed.push_back(std::move(name));
      }
    };
    note(a.gl.program == b.gl.program, "program");
    note(a.gl.arrayBuffer == b.gl.arrayBuffer, "array buffer");
    note(a.elementBuffer == b.elementBuffer, "element array buffer");
    note(a.framebuffer == b.framebuffer, "framebuffer");
    note(a.gl.activeTexture == b.gl.activeTexture, "active texture unit");
    note(a.textures == b.textures, "bound textures");
    note(a.gl.blend == b.gl.blend && a.gl.blendFunc == b.gl.blendFunc, "blending");
    note(a.blendEquation == b.blendEquation, "blend equation");
    note(a.blendColor == b.blendColor, "blend color");
    note(a.clearColor == b.clearColor, "clear color");
    note(a.lineWidth == b.lineWidth, "line width");
    note(a.depthMask == b.depthMask, "depth mask");
    note(a.gl.scissor == b.gl.scissor && a.gl.scissorBox == b.gl.scissorBox, "scissor");
    note(a.gl.viewport == b.gl.viewport, "viewport");
    note(a.gl.depthTest == b.gl.depthTest, "depth test");
    note(a.cullFace == b.cullFace && a.stencilTest == b.stencilTest, "culling or stencil");
    note(a.colorMask == b.colorMask, "color mask");
    note(a.unpackAlignment == b.unpackAlignment, "unpack alignment");
    for (std::size_t i = 0; i < std::min(a.gl.attributes.size(), b.gl.attributes.size()); ++i) {
      const auto& p = a.gl.attributes[i];
      const auto& q = b.gl.attributes[i];
      note(
          p.enabled == q.enabled
              && p.size == q.size
              && p.type == q.type
              && p.normalized == q.normalized
              && p.stride == q.stride
              && p.buffer == q.buffer
              && p.pointer == q.pointer,
          std::format("vertex attribute {}", i)
      );
    }
    return changed;
  }

  inline double threadCpuMilliseconds() {
    timespec now{};
    TEST_CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
    return (static_cast<double>(now.tv_sec) * 1e3) + (static_cast<double>(now.tv_nsec) / 1e6);
  }

  // Runs visuals in an offscreen OpenGL ES 2 context big enough for every capture target with a margin around it.
  // Before each run it sets the state a host leaves (see AudioVisual) with the rig's own program, buffers and
  // textures bound, texture unit 1 active, the scissor on over the whole framebuffer, and the framebuffer filled with
  // a color no visual uses; before each frame it fills the rectangle with the palette's background.
  class AudioVisualRig {
  public:
    static constexpr int kWidth = 1400;
    static constexpr int kHeight = 940;
    static constexpr int kMargin = 16;
    static constexpr int kMaxFrames = 3600;
    static constexpr Pixel kOutside{79, 158, 120, 255};

    // Empty with `reason` filled when the machine offers no offscreen context.
    static std::unique_ptr<AudioVisualRig> create(std::string& reason) {
      auto gl = GlTestContext::create(kWidth, kHeight, false, reason);
      if (!gl) {
        return nullptr;
      }
      return std::unique_ptr<AudioVisualRig>(new AudioVisualRig(std::move(gl)));
    }

    ~AudioVisualRig() {
      glDeleteBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
      glDeleteTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
    }

    AudioVisualRig(const AudioVisualRig&) = delete;
    AudioVisualRig& operator=(const AudioVisualRig&) = delete;

    // Advances and draws `visual` for `frames` frames of `source` at `target`, its rectangle kMargin buffer pixels
    // in from the top left corner. The visual and the source carry on from where they were.
    AudioVisualCapture run(AudioVisual& visual, AudioFrameSource& source, int frames, const AudioVisualTarget& target) {
      TEST_CHECK(frames >= 1 && frames <= kMaxFrames);
      TEST_CHECK(std::isfinite(target.scale) && target.scale > 0 && target.width >= 0 && target.height >= 0);
      const auto width = static_cast<int>(std::lround(target.width * target.scale));
      const auto height = static_cast<int>(std::lround(target.height * target.scale));
      TEST_CHECK(width + (2 * kMargin) <= kWidth && height + (2 * kMargin) <= kHeight);
      const AudioVisualDraw draw{
          .node =
              CustomDrawContext{
                  .surfaceWidth = kWidth / target.scale,
                  .surfaceHeight = kHeight / target.scale,
                  .pixelScaleX = target.scale,
                  .pixelScaleY = target.scale,
                  .transform = Mat3::translation(kMargin / target.scale, kMargin / target.scale),
                  .opacity = target.opacity,
                  .width = static_cast<float>(target.width),
                  .height = static_cast<float>(target.height),
              },
          .rect = {.x = kMargin, .y = kHeight - kMargin - height, .width = width, .height = height},
          .scale = target.scale,
          .palette = rigPalette(),
      };
      prepare();
      AudioVisualCapture capture;
      double total = 0;
      int timed = 0;
      for (int frame = 0; frame < frames; ++frame) {
        fillBackground(draw.rect);
        const auto before = captureRigState();
        const double start = threadCpuMilliseconds();
        visual.advance(source.next());
        visual.draw(draw);
        const double spent = threadCpuMilliseconds() - start;
        TEST_CHECK(glGetError() == GL_NO_ERROR);
        const auto changed = changedState(before, captureRigState());
        for (const auto& name : changed) {
          std::println(stderr, "frame {}: the draw left the {} changed", frame, name);
        }
        capture.stateKept = capture.stateKept && changed.empty();
        if (frame > 0 || frames == 1) {
          total += spent;
          ++timed;
          capture.maxCpuMilliseconds = std::max(capture.maxCpuMilliseconds, spent);
        }
      }
      capture.meanCpuMilliseconds = total / timed;
      const GlPixels all = m_gl->readPixels();
      capture.insideOnly = pixelsOtherThan(all, kOutside, 0, wholeRegion(all)).count
          == pixelsOtherThan(all, kOutside, 0, {kMargin, kMargin, kMargin + width, kMargin + height}).count;
      capture.pixels = crop(all, width, height);
      return capture;
    }

  private:
    explicit AudioVisualRig(std::unique_ptr<GlTestContext> gl) : m_gl(std::move(gl)) {
      m_program.create(
          "attribute vec3 a_point; void main() { gl_Position = vec4(a_point, 1.0); }",
          "precision mediump float; void main() { gl_FragColor = vec4(1.0); }"
      );
      glGenBuffers(static_cast<GLsizei>(m_buffers.size()), m_buffers.data());
      for (const GLuint buffer : m_buffers) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, 4096, nullptr, GL_STATIC_DRAW);
      }
      glGenTextures(static_cast<GLsizei>(m_textures.size()), m_textures.data());
      for (const GLuint texture : m_textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      }
      TEST_CHECK(glGetError() == GL_NO_ERROR);
    }

    void prepare() const {
      glViewport(0, 0, kWidth, kHeight);
      glEnable(GL_SCISSOR_TEST);
      glScissor(0, 0, kWidth, kHeight);
      glEnable(GL_BLEND);
      glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_CULL_FACE);
      glDisable(GL_STENCIL_TEST);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glUseProgram(m_program.id());
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffers[1]);
      glBindBuffer(GL_ARRAY_BUFFER, m_buffers[0]);
      GLint attributes = 0;
      glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
      for (GLint i = 1; i < std::clamp(attributes, 0, 64); ++i) {
        glDisableVertexAttribArray(static_cast<GLuint>(i));
      }
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, reinterpret_cast<const void*>(16));
      glEnableVertexAttribArray(0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, m_textures[0]);
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, m_textures[1]);
      glClearColor(kOutside[0] / 255.0F, kOutside[1] / 255.0F, kOutside[2] / 255.0F, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    static void fillBackground(const AudioVisualRect& rect) {
      if (rect.width <= 0 || rect.height <= 0) {
        return;
      }
      const Color& background = rigPalette().background;
      glScissor(rect.x, rect.y, rect.width, rect.height);
      glClearColor(background.r, background.g, background.b, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glScissor(0, 0, kWidth, kHeight);
    }

    static GlPixels crop(const GlPixels& all, int width, int height) {
      GlPixels out{.width = width, .height = height, .rgba = {}};
      out.rgba.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
      for (int y = kMargin; y < kMargin + height; ++y) {
        const auto row = all.rgba.begin() + ((static_cast<std::ptrdiff_t>(y) * all.width + kMargin) * 4);
        out.rgba.insert(out.rgba.end(), row, row + (static_cast<std::ptrdiff_t>(width) * 4));
      }
      return out;
    }

    std::unique_ptr<GlTestContext> m_gl;
    ShaderProgram m_program;
    std::array<GLuint, 2> m_buffers{};
    std::array<GLuint, 2> m_textures{};
  };
} // namespace test::audio
