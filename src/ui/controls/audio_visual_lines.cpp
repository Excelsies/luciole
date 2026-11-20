#include "ui/controls/audio_visual_lines.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>

AudioRowMesh::AudioRowMesh(Shape shape, const char* vertexShader, const char* fragmentShader)
    : m_shape(shape), m_vertexShader(vertexShader), m_fragmentShader(fragmentShader) {
  if (shape.rows == 0
      || shape.verticesPerRow == 0
      || shape.indicesPerRow == 0
      || shape.valuesPerVertex == 0
      || shape.valuesPerVertex > 4
      || shape.rows * shape.verticesPerRow > std::size_t{std::numeric_limits<GLushort>::max()} + 1) {
    throw std::invalid_argument("a sound visual's row mesh has an impossible shape");
  }
  m_values.assign(shape.rows * shape.verticesPerRow * shape.valuesPerVertex, 0.0F);
}

void AudioRowMesh::create(std::span<const Place> places, std::span<const GLushort> indices) {
  const std::size_t vertices = m_shape.rows * m_shape.verticesPerRow;
  if (places.size() != vertices
      || indices.size() != m_shape.rows * m_shape.indicesPerRow
      || std::ranges::any_of(indices, [&](GLushort index) { return index >= vertices; })) {
    throw std::invalid_argument("a sound visual's row mesh got data of the wrong size");
  }
  release();
  m_program.create(m_vertexShader, m_fragmentShader);
  m_placeLocation = glGetAttribLocation(m_program.id(), "a_place");
  m_valueLocation = glGetAttribLocation(m_program.id(), "a_value");
  std::array<GLuint, 3> buffers{};
  glGenBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
  m_placeBuffer = buffers[0];
  m_valueBuffer = buffers[1];
  m_indexBuffer = buffers[2];
  glBindBuffer(GL_ARRAY_BUFFER, m_placeBuffer);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(places.size_bytes()), places.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, m_valueBuffer);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_values.size() * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_indexBuffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size_bytes()), indices.data(), GL_STATIC_DRAW);
  m_stale = true;
}

GLint AudioRowMesh::uniform(const char* name) const { return glGetUniformLocation(m_program.id(), name); }

void AudioRowMesh::bind(AudioVisualGlState& saved) const {
  saved.keepAttributes(std::array{m_placeLocation, m_valueLocation});
  glUseProgram(m_program.id());
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_indexBuffer);
  if (m_placeLocation >= 0) {
    const auto location = static_cast<GLuint>(m_placeLocation);
    glBindBuffer(GL_ARRAY_BUFFER, m_placeBuffer);
    glVertexAttribPointer(location, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Place), nullptr);
    glEnableVertexAttribArray(location);
  }
  glBindBuffer(GL_ARRAY_BUFFER, m_valueBuffer);
  if (m_valueLocation >= 0) {
    const auto location = static_cast<GLuint>(m_valueLocation);
    glVertexAttribPointer(location, static_cast<GLint>(m_shape.valuesPerVertex), GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(location);
  }
}

void AudioRowMesh::upload(std::size_t firstRow, std::size_t rowCount) const {
  const std::size_t rowValues = m_shape.verticesPerRow * m_shape.valuesPerVertex;
  const auto part = std::span(m_values).subspan(firstRow * rowValues, rowCount * rowValues);
  glBindBuffer(GL_ARRAY_BUFFER, m_valueBuffer);
  glBufferSubData(
      GL_ARRAY_BUFFER, static_cast<GLintptr>(firstRow * rowValues * sizeof(float)),
      static_cast<GLsizeiptr>(part.size_bytes()), part.data()
  );
}

void AudioRowMesh::drawOldestFirst(GLenum mode, std::size_t head) const {
  const auto draw = [&](std::size_t first, std::size_t count) {
    if (count == 0) {
      return;
    }
    const std::size_t offset = first * m_shape.indicesPerRow * sizeof(GLushort);
    glDrawElements(
        mode, static_cast<GLsizei>(count * m_shape.indicesPerRow), GL_UNSIGNED_SHORT,
        reinterpret_cast<const void*>(offset)
    );
  };
  head %= m_shape.rows;
  draw(head + 1, m_shape.rows - head - 1);
  draw(0, head + 1);
}

void AudioRowMesh::release() {
  m_program.destroy();
  const std::array<GLuint, 3> buffers{m_placeBuffer, m_valueBuffer, m_indexBuffer};
  if (std::ranges::any_of(buffers, [](GLuint buffer) { return buffer != 0; })) {
    glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
  }
  abandon();
}

void AudioRowMesh::abandon() noexcept {
  m_program.abandon();
  m_placeBuffer = 0;
  m_valueBuffer = 0;
  m_indexBuffer = 0;
  m_placeLocation = -1;
  m_valueLocation = -1;
  m_stale = true;
}
