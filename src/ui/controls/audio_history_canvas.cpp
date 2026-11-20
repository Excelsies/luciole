#include "ui/controls/audio_history_canvas.h"

#include <stdexcept>

namespace {
  using luciole::workbench::AudioHistory;
  using luciole::workbench::HistoryChange;

  constexpr char kVertexShader[] = R"(
attribute vec2 a_corner;
uniform vec2 u_extent;
uniform vec2 u_span;
varying vec2 v_point;

void main() {
    v_point = a_corner * u_span;
    gl_Position = vec4(a_corner * u_extent, 0.0, 1.0);
}
)";

  constexpr std::array<float, 8> kCorners{-1, -1, 1, -1, -1, 1, 1, 1};

  bool isPowerOfTwo(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }
} // namespace

AudioHistoryCanvas::AudioHistoryCanvas(const char* fragmentShader, HistorySampling sampling)
    : m_fragmentShader(fragmentShader), m_sampling(sampling) {}

AudioHistoryCanvas::~AudioHistoryCanvas() { release(); }

bool AudioHistoryCanvas::begin(AudioVisualGlState& saved, const AudioVisualRect& rect, AudioHistory& history) {
  bool made = false;
  if (!m_program.isValid()) {
    m_program.create(kVertexShader, m_fragmentShader);
    m_corner = glGetAttribLocation(m_program.id(), "a_corner");
    m_extent = glGetUniformLocation(m_program.id(), "u_extent");
    m_span = glGetUniformLocation(m_program.id(), "u_span");
    made = true;
  }
  saved.keepAttributes(std::array{m_corner});
  if (m_corners == 0) {
    glGenBuffers(1, &m_corners);
    glBindBuffer(GL_ARRAY_BUFFER, m_corners);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCorners), kCorners.data(), GL_STATIC_DRAW);
  }
  glViewport(rect.x, rect.y, rect.width, rect.height);
  glUseProgram(m_program.id());
  if (made) {
    glUniform1i(uniform("u_history"), 0);
  }
  if (m_texture == 0 || m_width != history.width() || m_rows != history.rows()) {
    makeTexture(history);
    (void)history.takeUpdate();
    return made;
  }
  glBindTexture(GL_TEXTURE_2D, m_texture);
  const auto update = history.takeUpdate();
  const auto width = static_cast<GLsizei>(m_width);
  if (update.change == HistoryChange::Row) {
    const std::size_t stride = m_width * AudioHistory::kBytesPerTexel;
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, static_cast<GLint>(update.row), width, 1, GL_RGBA, GL_UNSIGNED_BYTE,
        history.texels().subspan(update.row * stride, stride).data()
    );
  } else if (update.change == HistoryChange::All) {
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, width, static_cast<GLsizei>(m_rows), GL_RGBA, GL_UNSIGNED_BYTE, history.texels().data()
    );
  }
  return made;
}

GLint AudioHistoryCanvas::uniform(const char* name) const { return glGetUniformLocation(m_program.id(), name); }

void AudioHistoryCanvas::drawSquare(std::array<float, 2> extent, std::array<float, 2> span) const {
  glUniform2f(m_extent, extent[0], extent[1]);
  glUniform2f(m_span, span[0], span[1]);
  const auto corner = static_cast<GLuint>(m_corner);
  glBindBuffer(GL_ARRAY_BUFFER, m_corners);
  glVertexAttribPointer(corner, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(corner);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void AudioHistoryCanvas::makeTexture(const AudioHistory& history) {
  const bool repeat = m_sampling == HistorySampling::SmoothRepeat;
  if (repeat && (!isPowerOfTwo(history.width()) || !isPowerOfTwo(history.rows()))) {
    throw std::invalid_argument("a repeating history texture needs sides that are powers of two");
  }
  if (m_texture == 0) {
    glGenTextures(1, &m_texture);
  }
  m_width = history.width();
  m_rows = history.rows();
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_rows), 0, GL_RGBA,
      GL_UNSIGNED_BYTE, history.texels().data()
  );
}

void AudioHistoryCanvas::release() {
  m_program.destroy();
  if (m_corners != 0) {
    glDeleteBuffers(1, &m_corners);
    m_corners = 0;
  }
  if (m_texture != 0) {
    glDeleteTextures(1, &m_texture);
    m_texture = 0;
  }
  m_width = 0;
  m_rows = 0;
}

void AudioHistoryCanvas::abandon() noexcept {
  m_program.abandon();
  m_corners = 0;
  m_texture = 0;
  m_width = 0;
  m_rows = 0;
}
