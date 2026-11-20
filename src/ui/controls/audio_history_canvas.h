#pragma once

#include "render/core/shader_program.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_history.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstddef>
#include <cstdint>

// How a history texture is read: filtered and clamped at its edges, or filtered and repeating, which needs sides that
// are powers of two.
enum class HistorySampling : std::uint8_t { Smooth, SmoothRepeat };

// What a sound visual drawn by one fragment shader over a history texture shares: the program, a square of corners
// and the texture that mirrors an AudioHistory, made on first use. The fragment shader reads the texture as
// `uniform sampler2D u_history` on unit 0 and receives `varying vec2 v_point`, the corner scaled by the span given to
// drawSquare().
class AudioHistoryCanvas {
public:
  // `fragmentShader` outlives the canvas.
  AudioHistoryCanvas(const char* fragmentShader, HistorySampling sampling);
  ~AudioHistoryCanvas();
  AudioHistoryCanvas(const AudioHistoryCanvas&) = delete;
  AudioHistoryCanvas& operator=(const AudioHistoryCanvas&) = delete;

  // Makes what is missing, sets the viewport to `rect`, puts the program in use with the texture on unit 0, and
  // uploads what changed in `history` since the last call: one row, or all of it when the texture is new. Returns
  // true when the program was just made, so the caller looks up its uniforms. Throws std::runtime_error when the
  // program does not compile, and std::invalid_argument when SmoothRepeat meets a side that is not a power of two.
  bool begin(AudioVisualGlState& saved, const AudioVisualRect& rect, luciole::workbench::AudioHistory& history);
  GLint uniform(const char* name) const;
  // Draws the share `extent` of the viewport around its center, with v_point running from -span to span.
  void drawSquare(std::array<float, 2> extent, std::array<float, 2> span) const;

  void release();
  void abandon() noexcept;

  GLuint program() const { return m_program.id(); }
  GLuint texture() const { return m_texture; }
  GLuint buffer() const { return m_corners; }

private:
  void makeTexture(const luciole::workbench::AudioHistory& history);

  const char* m_fragmentShader;
  HistorySampling m_sampling;
  ShaderProgram m_program;
  GLint m_corner = -1;
  GLint m_extent = -1;
  GLint m_span = -1;
  GLuint m_corners = 0;
  GLuint m_texture = 0;
  std::size_t m_width = 0;
  std::size_t m_rows = 0;
};
