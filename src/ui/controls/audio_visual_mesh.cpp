#include "ui/controls/audio_visual_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

void AudioVisualMesh::ensureProgram(const char* vertexShader, const char* fragmentShader) {
  if (m_program.isValid()) {
    return;
  }
  m_program.create(vertexShader, fragmentShader);
  m_vertex = glGetAttribLocation(m_program.id(), "a_vertex");
  if (m_vertex < 0) {
    release();
    throw std::runtime_error("a sound visual's program has no a_vertex attribute");
  }
}

GLint AudioVisualMesh::uniform(const char* name) {
  const GLint location = glGetUniformLocation(m_program.id(), name);
  if (location < 0) {
    release();
    throw std::runtime_error(std::string("a sound visual's program has no uniform ") + name);
  }
  return location;
}

void AudioVisualMesh::fill(std::uint64_t shape, std::span<const AudioVisualVertex> vertices) {
  if (m_buffer == 0) {
    glGenBuffers(1, &m_buffer);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
  const std::size_t count = std::min<std::size_t>(vertices.size(), std::numeric_limits<GLsizei>::max());
  glBufferData(
      GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(AudioVisualVertex)), vertices.data(), GL_STATIC_DRAW
  );
  m_count = static_cast<GLsizei>(count);
  m_shape = shape;
}

void AudioVisualMesh::bind() const {
  glUseProgram(m_program.id());
  glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
  const auto location = static_cast<GLuint>(m_vertex);
  glVertexAttribPointer(location, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(AudioVisualVertex), nullptr);
  glEnableVertexAttribArray(location);
}

void AudioVisualMesh::release() {
  m_program.destroy();
  if (m_buffer != 0) {
    glDeleteBuffers(1, &m_buffer);
  }
  abandon();
}

void AudioVisualMesh::abandon() noexcept {
  m_program.abandon();
  m_vertex = -1;
  m_buffer = 0;
  m_count = 0;
  m_shape = 0;
}

float audioVisualOpacity(const AudioVisualDraw& draw) {
  return std::isnan(draw.node.opacity) ? 0.0F : std::clamp(draw.node.opacity, 0.0F, 1.0F);
}

void setColorUniform(GLint location, const Color& color) { glUniform3f(location, color.r, color.g, color.b); }
