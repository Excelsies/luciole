#include "gl_test_context.h"
#include "test_check.h"
#include "ui/controls/audio_history_canvas.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_history.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>
#include <print>
#include <stdexcept>
#include <string>

namespace {
  using luciole::workbench::AudioHistory;
  using luciole::workbench::HistoryChange;

  constexpr int kSide = 8;

  // Shows the history texture over the whole square.
  constexpr char kShowHistory[] = R"(
precision mediump float;
uniform sampler2D u_history;
varying vec2 v_point;
void main() {
    gl_FragColor = texture2D(u_history, v_point * 0.5 + 0.5);
}
)";

  constexpr char kBroken[] = "precision mediump float; void main() { gl_FragColor = undefined; }";

  void paintRow(AudioHistory& history, std::size_t row, std::array<std::uint8_t, 4> color) {
    const auto bytes = history.writeRow(row);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = color[i % 4];
    }
  }

  // Draws into the bottom left `side` by `side` pixels, one pixel per texel for a history of that size, so the
  // filter reads texel centers.
  void draw(AudioHistoryCanvas& canvas, AudioHistory& history, std::array<float, 2> extent, int side) {
    AudioVisualGlState saved;
    (void)canvas.begin(saved, {.x = 0, .y = 0, .width = side, .height = side}, history);
    canvas.drawSquare(extent, {1, 1});
  }

  // Texture row 0 is the bottom of the drawing; the image's rows run from the top.
  std::array<std::uint8_t, 4> pixelAt(const test::GlPixels& pixels, int x, int y) { return pixels.at(x, y); }

  void showsWhatWasWrittenAndUploadsOnce(test::GlTestContext& gl) {
    AudioHistoryCanvas canvas(kShowHistory, HistorySampling::Smooth);
    AudioHistory history(4, 4);
    paintRow(history, 1, {255, 0, 0, 255});
    gl.clear(0, 0, 0, 0);
    draw(canvas, history, {1, 1}, 4);
    TEST_CHECK(history.takeUpdate().change == HistoryChange::None);
    auto pixels = gl.readPixels();
    // Image row 6 is texture row 1 of the four drawn at the bottom of the eight.
    TEST_CHECK((pixelAt(pixels, 1, 6) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    TEST_CHECK((pixelAt(pixels, 1, 7) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    TEST_CHECK((pixelAt(pixels, 1, 5) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    // One more row reaches the texture without the first being lost.
    paintRow(history, 3, {0, 0, 255, 255});
    gl.clear(0, 0, 0, 0);
    draw(canvas, history, {1, 1}, 4);
    pixels = gl.readPixels();
    TEST_CHECK((pixelAt(pixels, 1, 6) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
    TEST_CHECK((pixelAt(pixels, 3, 4) == std::array<std::uint8_t, 4>{0, 0, 255, 255}));
    // Nothing is drawn past the rectangle.
    TEST_CHECK((pixelAt(pixels, 5, 6) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    canvas.release();
  }

  void extentLimitsTheSquare(test::GlTestContext& gl) {
    AudioHistoryCanvas canvas(kShowHistory, HistorySampling::Smooth);
    AudioHistory history(1, 1);
    paintRow(history, 0, {0, 255, 0, 255});
    gl.clear(0, 0, 0, 0);
    draw(canvas, history, {0.5F, 0.5F}, kSide);
    const auto pixels = gl.readPixels();
    TEST_CHECK((pixelAt(pixels, 3, 3) == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
    TEST_CHECK((pixelAt(pixels, 1, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    TEST_CHECK((pixelAt(pixels, 6, 4) == std::array<std::uint8_t, 4>{0, 0, 0, 0}));
    canvas.release();
  }

  void releaseFreesAndAbandonForgets(test::GlTestContext& gl) {
    AudioHistoryCanvas canvas(kShowHistory, HistorySampling::SmoothRepeat);
    AudioHistory history(4, 8);
    draw(canvas, history, {1, 1}, kSide);
    const GLuint program = canvas.program();
    const GLuint texture = canvas.texture();
    const GLuint buffer = canvas.buffer();
    TEST_CHECK(glIsProgram(program) == GL_TRUE && glIsTexture(texture) == GL_TRUE && glIsBuffer(buffer) == GL_TRUE);
    canvas.release();
    TEST_CHECK(glIsProgram(program) == GL_FALSE && glIsTexture(texture) == GL_FALSE && glIsBuffer(buffer) == GL_FALSE);
    TEST_CHECK(canvas.program() == 0 && canvas.texture() == 0 && canvas.buffer() == 0);
    // Drawn again, it makes everything anew from the history in memory.
    paintRow(history, 0, {9, 9, 9, 255});
    (void)history.takeUpdate();
    gl.clear(0, 0, 0, 0);
    draw(canvas, history, {1, 1}, kSide);
    TEST_CHECK(canvas.program() != 0 && canvas.texture() != 0);
    const GLuint kept = canvas.texture();
    canvas.abandon();
    TEST_CHECK(canvas.program() == 0 && canvas.texture() == 0 && canvas.buffer() == 0);
    TEST_CHECK(glIsTexture(kept) == GL_TRUE);
    glDeleteTextures(1, &kept);
  }

  void refusesWhatItCannotDraw() {
    const auto before = test::captureGlState();
    bool refused = false;
    try {
      AudioHistoryCanvas canvas(kShowHistory, HistorySampling::SmoothRepeat);
      AudioHistory history(6, 4);
      draw(canvas, history, {1, 1}, kSide);
    } catch (const std::invalid_argument&) {
      refused = true;
    }
    TEST_CHECK(refused);
    refused = false;
    try {
      AudioHistoryCanvas canvas(kBroken, HistorySampling::Smooth);
      AudioHistory history(4, 4);
      draw(canvas, history, {1, 1}, kSide);
    } catch (const std::runtime_error&) {
      refused = true;
    }
    TEST_CHECK(refused);
    const auto after = test::captureGlState();
    TEST_CHECK(before.program == after.program && before.arrayBuffer == after.arrayBuffer);
    TEST_CHECK(before.viewport == after.viewport && before.texture == after.texture);
    TEST_CHECK(glGetError() == GL_NO_ERROR);
  }
} // namespace

int main() {
  std::string reason;
  auto gl = test::GlTestContext::create(kSide, kSide, false, reason);
  if (!gl) {
    std::println("skipped: {}", reason);
    return test::kSkipExitCode;
  }
  glDisable(GL_BLEND);
  showsWhatWasWrittenAndUploadsOnce(*gl);
  extentLimitsTheSquare(*gl);
  releaseFreesAndAbandonForgets(*gl);
  refusesWhatItCannotDraw();
  std::println("audio history canvas checks passed");
  return 0;
}
