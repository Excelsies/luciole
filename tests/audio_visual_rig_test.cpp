#include "audio_visual_rig.h"
#include "render/core/shader_program.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_frame.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using luciole::workbench::AudioFrame;
  using test::audio::AudioFrameSource;
  using test::audio::AudioScene;
  using test::audio::AudioVisualRig;

  // Fills the left `fraction` of its rectangle with the first accent. `leak` draws over the whole viewport the host
  // left instead, and `careless` skips restoring the GL state: the rig must catch both.
  class FillVisual final : public AudioVisual {
  public:
    struct Options {
      float fraction = 1;
      bool leak = false;
      bool careless = false;
    };

    explicit FillVisual(Options options) : m_options(options) {}
    ~FillVisual() override { release(); }

    void reset() override {
      m_advances = 0;
      m_hits = 0;
    }

    void advance(const AudioFrame& frame) override {
      ++m_advances;
      m_hits += frame.hit ? 1 : 0;
    }

    void draw(const AudioVisualDraw& draw) override {
      if (draw.rect.width <= 0 || draw.rect.height <= 0) {
        return;
      }
      if (m_options.careless) {
        paint(draw);
        return;
      }
      AudioVisualGlState saved;
      ensureObjects();
      saved.keepAttributes(std::array{m_position});
      paint(draw);
    }

    void release() override {
      m_program.destroy();
      if (m_buffer != 0) {
        glDeleteBuffers(1, &m_buffer);
        m_buffer = 0;
      }
    }

    void abandon() noexcept override {
      m_program.abandon();
      m_buffer = 0;
    }

    int advances() const { return m_advances; }
    int hits() const { return m_hits; }
    GLuint program() const { return m_program.id(); }
    GLuint buffer() const { return m_buffer; }

  private:
    void ensureObjects() {
      if (m_program.isValid()) {
        return;
      }
      m_program.create(
          "attribute vec2 a_position; void main() { gl_Position = vec4(a_position, 0.0, 1.0); }",
          "precision mediump float; uniform vec4 u_color; void main() { gl_FragColor = u_color; }"
      );
      m_position = glGetAttribLocation(m_program.id(), "a_position");
      m_color = glGetUniformLocation(m_program.id(), "u_color");
      TEST_CHECK(m_position >= 0 && m_color >= 0);
      const float right = (2 * m_options.fraction) - 1;
      const std::array<float, 12> corners{-1, -1, right, -1, -1, 1, right, -1, right, 1, -1, 1};
      glGenBuffers(1, &m_buffer);
      glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
      glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners.data(), GL_STATIC_DRAW);
    }

    void paint(const AudioVisualDraw& draw) {
      ensureObjects();
      if (!m_options.leak) {
        glViewport(draw.rect.x, draw.rect.y, draw.rect.width, draw.rect.height);
      }
      const Color& color = draw.palette.accents[0];
      const float alpha = color.a * draw.node.opacity;
      glUseProgram(m_program.id());
      glUniform4f(m_color, color.r * alpha, color.g * alpha, color.b * alpha, alpha);
      glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
      const auto location = static_cast<GLuint>(m_position);
      glVertexAttribPointer(location, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
      glEnableVertexAttribArray(location);
      glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    Options m_options;
    ShaderProgram m_program;
    GLuint m_buffer = 0;
    GLint m_position = -1;
    GLint m_color = -1;
    int m_advances = 0;
    int m_hits = 0;
  };

  void fillsItsRectangleAtEverySize(AudioVisualRig& rig) {
    const auto accent = test::audio::pixelOf(test::audio::rigPalette().accents[0]);
    for (const auto& size : test::audio::kCaptureTargets) {
      FillVisual visual({});
      AudioFrameSource source(AudioScene::Tone);
      const auto capture = rig.run(visual, source, 30, size);
      TEST_CHECK(capture.stateKept);
      TEST_CHECK(capture.insideOnly);
      TEST_CHECK(capture.pixels.width == static_cast<int>(std::lround(size.width * size.scale)));
      TEST_CHECK(capture.pixels.height == static_cast<int>(std::lround(size.height * size.scale)));
      const auto whole = test::audio::wholeRegion(capture.pixels);
      TEST_CHECK(test::audio::pixelsOtherThan(capture.pixels, accent, 1, whole).count == 0);
      TEST_CHECK(visual.advances() == 30);
      TEST_CHECK(capture.meanCpuMilliseconds > 0);
      std::println(
          "fill {}x{} at {}: {:.4f} ms mean, {:.4f} ms max processor time per frame", size.width, size.height,
          size.scale, capture.meanCpuMilliseconds, capture.maxCpuMilliseconds
      );
      test::audio::keepForViewing(capture.pixels, test::audio::captureName("rig-fill", "tone", size));
    }
  }

  void findsWherePixelsAre(AudioVisualRig& rig) {
    const test::audio::AudioVisualTarget size{.width = 400, .height = 200, .scale = 1};
    FillVisual visual({.fraction = 0.25F});
    AudioFrameSource source(AudioScene::Silence);
    const auto capture = rig.run(visual, source, 2, size);
    const auto whole = test::audio::wholeRegion(capture.pixels);
    const auto background = test::audio::pixelOf(test::audio::rigPalette().background);
    const auto lit = test::audio::pixelsOtherThan(capture.pixels, background, 2, whole);
    TEST_CHECK(lit.count == 100 * 200);
    TEST_NEAR(lit.centerX, 49.5, 0.01);
    TEST_NEAR(lit.centerY, 99.5, 0.01);
    // The first accent is a pale yellow, far brighter than the background.
    const auto bright = test::audio::brightPixels(capture.pixels, 160, whole);
    TEST_CHECK(bright.count == lit.count);
    const test::audio::Region right{.left = 100, .top = 0, .right = 400, .bottom = 200};
    TEST_CHECK(test::audio::brightPixels(capture.pixels, 160, right).count == 0);
    TEST_CHECK(test::audio::pixelsOtherThan(capture.pixels, background, 2, right).count == 0);
    TEST_CHECK(test::audio::luma(background) < 40);
  }

  void catchesLeaksAndLostState(AudioVisualRig& rig) {
    const test::audio::AudioVisualTarget size{.width = 300, .height = 200, .scale = 1};
    {
      FillVisual visual({.leak = true});
      AudioFrameSource source(AudioScene::Tone);
      const auto capture = rig.run(visual, source, 3, size);
      TEST_CHECK(!capture.insideOnly);
      TEST_CHECK(capture.stateKept);
    }
    {
      FillVisual visual({.careless = true});
      AudioFrameSource source(AudioScene::Tone);
      const auto capture = rig.run(visual, source, 3, size);
      TEST_CHECK(!capture.stateKept);
      TEST_CHECK(capture.insideOnly);
    }
  }

  void sameFramesSamePixels(AudioVisualRig& rig) {
    const test::audio::AudioVisualTarget size{.width = 320, .height = 240, .scale = 1.5F};
    FillVisual first({.fraction = 0.6F});
    FillVisual second({.fraction = 0.6F});
    AudioFrameSource firstSource(AudioScene::Noise);
    AudioFrameSource secondSource(AudioScene::Noise);
    const auto a = rig.run(first, firstSource, 20, size);
    const auto b = rig.run(second, secondSource, 20, size);
    TEST_CHECK(test::audio::differingPixels(a.pixels, b.pixels, 0) == 0);
    FillVisual wider({.fraction = 0.7F});
    AudioFrameSource widerSource(AudioScene::Noise);
    const auto c = rig.run(wider, widerSource, 20, size);
    // A tenth of 480 by 360 pixels.
    TEST_CHECK(test::audio::differingPixels(a.pixels, c.pixels, 0) == 48 * 360);
  }

  void runsContinueAndRelease(AudioVisualRig& rig) {
    const test::audio::AudioVisualTarget size{.width = 200, .height = 100, .scale = 1};
    FillVisual visual({});
    AudioFrameSource source(AudioScene::Kicks);
    (void)rig.run(visual, source, 40, size);
    TEST_CHECK(visual.hits() == 2);
    (void)rig.run(visual, source, 21, size);
    TEST_CHECK(source.frames() == 61);
    TEST_CHECK(visual.hits() == 3);
    TEST_CHECK(visual.advances() == 61);

    const GLuint program = visual.program();
    const GLuint buffer = visual.buffer();
    TEST_CHECK(glIsProgram(program) == GL_TRUE && glIsBuffer(buffer) == GL_TRUE);
    visual.release();
    TEST_CHECK(glIsProgram(program) == GL_FALSE && glIsBuffer(buffer) == GL_FALSE);
    visual.reset();
    const auto again = rig.run(visual, source, 1, size);
    TEST_CHECK(again.stateKept && again.insideOnly);
    TEST_CHECK(visual.advances() == 1);
    const auto accent = test::audio::pixelOf(test::audio::rigPalette().accents[0]);
    TEST_CHECK(
        test::audio::pixelsOtherThan(again.pixels, accent, 1, test::audio::wholeRegion(again.pixels)).count == 0
    );
  }

  // Everything a visual may change comes back when the guard goes; a fifth attribute array is refused rather than
  // left unrestored.
  void guardPutsStateBack() {
    const auto before = test::audio::captureRigState();
    TEST_CHECK(before.gl.activeTexture == GL_TEXTURE1);
    std::array<GLuint, 2> buffers{};
    GLuint texture = 0;
    glGenBuffers(2, buffers.data());
    glGenTextures(1, &texture);
    {
      AudioVisualGlState saved;
      saved.keepAttributes(std::array<GLint, 3>{0, -1, 1});
      saved.keepAttributes(std::array<GLint, 2>{2, 3});
      bool refused = false;
      try {
        saved.keepAttributes(std::array<GLint, 1>{4});
      } catch (const std::length_error&) {
        refused = true;
      }
      TEST_CHECK(refused);
      GLint active = 0;
      glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
      TEST_CHECK(active == GL_TEXTURE0);
      glViewport(3, 4, 50, 60);
      glUseProgram(0);
      glBindTexture(GL_TEXTURE_2D, texture);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
      glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
      for (const GLuint location : {0U, 1U, 2U, 3U}) {
        glVertexAttribPointer(location, 2, GL_UNSIGNED_BYTE, GL_TRUE, 8, nullptr);
        glEnableVertexAttribArray(location);
      }
      glActiveTexture(GL_TEXTURE2);
      TEST_CHECK(!test::audio::changedState(before, test::audio::captureRigState()).empty());
    }
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    TEST_CHECK(test::audio::changedState(before, test::audio::captureRigState()).empty());
    glDeleteBuffers(2, buffers.data());
    glDeleteTextures(1, &texture);
  }

  // A location kept again after it changed, as a helper shared by two draw calls does, comes back as it was before
  // the first keep and takes one place.
  void guardKeepsTheFirstSaveOfALocation() {
    const auto before = test::audio::captureRigState();
    TEST_CHECK(before.gl.attributes.at(0).enabled != 0 && before.gl.attributes.at(0).size == 3);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    {
      AudioVisualGlState saved;
      saved.keepAttributes(std::array<GLint, 1>{0});
      glBindBuffer(GL_ARRAY_BUFFER, buffer);
      glVertexAttribPointer(0, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, nullptr);
      glDisableVertexAttribArray(0);
      for (std::size_t keep = 0; keep < AudioVisualGlState::kMaxAttributes; ++keep) {
        saved.keepAttributes(std::array<GLint, 2>{0, 0});
      }
      saved.keepAttributes(std::array<GLint, 3>{1, 2, 3});
    }
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    const auto changed = test::audio::changedState(before, test::audio::captureRigState());
    for (const auto& name : changed) {
      std::println(stderr, "the guard left the {} changed", name);
    }
    TEST_CHECK(changed.empty());
    glDeleteBuffers(1, &buffer);
  }

  // Changes `name` with `change`, puts it back with `undo`, and says whether the rig saw the change.
  bool rigNotices(std::string_view name, const std::function<void()>& change, const std::function<void()>& undo) {
    const auto before = test::audio::captureRigState();
    change();
    const auto changed = test::audio::changedState(before, test::audio::captureRigState());
    undo();
    TEST_CHECK(test::audio::changedState(before, test::audio::captureRigState()).empty());
    return std::ranges::find(changed, name) != changed.end();
  }

  // State a visual must not change, read alongside the bindings.
  void rigNoticesFixedFunctionState() {
    GLfloat width = 0;
    glGetFloatv(GL_LINE_WIDTH, &width);
    TEST_CHECK(rigNotices("line width", [] { glLineWidth(2); }, [&] { glLineWidth(width); }));
    GLboolean depthMask = GL_FALSE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    TEST_CHECK(rigNotices(
        "depth mask", [&] { glDepthMask(depthMask == GL_TRUE ? GL_FALSE : GL_TRUE); }, [&] { glDepthMask(depthMask); }
    ));
    std::array<GLfloat, 4> clear{};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear.data());
    TEST_CHECK(rigNotices(
        "clear color", [] { glClearColor(0.25F, 0.5F, 0.75F, 0.5F); },
        [&] { glClearColor(clear[0], clear[1], clear[2], clear[3]); }
    ));
    std::array<GLfloat, 4> blend{};
    glGetFloatv(GL_BLEND_COLOR, blend.data());
    TEST_CHECK(rigNotices(
        "blend color", [] { glBlendColor(0.25F, 0.5F, 0.75F, 0.5F); },
        [&] { glBlendColor(blend[0], blend[1], blend[2], blend[3]); }
    ));
  }

  void emptyRectangleDrawsNothing(AudioVisualRig& rig) {
    FillVisual visual({.leak = true});
    AudioFrameSource source(AudioScene::Hostile);
    for (const auto& size :
         {test::audio::AudioVisualTarget{.width = 0, .height = 0, .scale = 1},
          test::audio::AudioVisualTarget{.width = 300, .height = 0, .scale = 1.5F}}) {
      const auto capture = rig.run(visual, source, 4, size);
      TEST_CHECK(capture.pixels.rgba.empty());
      TEST_CHECK(capture.insideOnly);
      TEST_CHECK(capture.stateKept);
    }
  }
} // namespace

int main() {
  std::string reason;
  auto rig = AudioVisualRig::create(reason);
  if (!rig) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  fillsItsRectangleAtEverySize(*rig);
  findsWherePixelsAre(*rig);
  catchesLeaksAndLostState(*rig);
  sameFramesSamePixels(*rig);
  runsContinueAndRelease(*rig);
  emptyRectangleDrawsNothing(*rig);
  guardPutsStateBack();
  guardKeepsTheFirstSaveOfALocation();
  rigNoticesFixedFunctionState();
  std::println("audio visual rig checks passed");
  return 0;
}
