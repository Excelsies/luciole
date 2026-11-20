#pragma once

#include "render/core/shader_program.h"
#include "ui/controls/audio_visual.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The GL side of a sound visual drawn from a ring of rows, one waveform ring or one ridge line per row: one program,
// a static vertex buffer giving every vertex its row and its place in the row as four bytes (attribute a_place), a
// dynamic vertex buffer of float values per vertex that takes only the rows pushed since the last draw (attribute
// a_value), and an index buffer with the same number of indices for every row, rows in order.
class AudioRowMesh {
public:
  struct Shape {
    std::size_t rows = 0;
    std::size_t verticesPerRow = 0;
    std::size_t valuesPerVertex = 0; // 1 to 4
    std::size_t indicesPerRow = 0;
  };
  using Place = std::array<std::uint8_t, 4>;

  // Throws std::invalid_argument for a shape without rows, vertices or indices, with more than four values per
  // vertex, or with more vertices than 16-bit indices reach.
  AudioRowMesh(Shape shape, const char* vertexShader, const char* fragmentShader);
  AudioRowMesh(const AudioRowMesh&) = delete;
  AudioRowMesh& operator=(const AudioRowMesh&) = delete;

  // Whether the GL objects exist.
  bool ready() const { return m_program.isValid(); }
  // Makes the program and the buffers from one place per vertex and the indices of every row, rows in order. Every
  // row of values is uploaded at the next sync. Throws std::runtime_error when the program does not compile and
  // std::invalid_argument for data of the wrong size or an index past the vertices.
  void create(std::span<const Place> places, std::span<const GLushort> indices);
  // The location of a uniform of the program; -1 when the program has none of that name.
  GLint uniform(const char* name) const;
  // Uses the program and points a_place and a_value at the buffers, keeping their state in `saved` first.
  void bind(AudioVisualGlState& saved) const;
  // Marks every row as changed, for a visual that forgot what it heard: the next sync uploads them all.
  void invalidate() { m_stale = true; }
  // Uploads the rows pushed since the last sync, `head` being the newest row and `pushes` the count since the
  // visual's reset: every row after create or invalidate, or when more were pushed than there are rows.
  // `fill(row, out)` writes one row's verticesPerRow * valuesPerVertex values. Needs the buffers bound by bind().
  template <class Fill> void sync(std::uint64_t pushes, std::size_t head, Fill&& fill) {
    const std::size_t rowValues = m_shape.verticesPerRow * m_shape.valuesPerVertex;
    const auto rowSpan = [&](std::size_t row) { return std::span(m_values).subspan(row * rowValues, rowValues); };
    if (m_stale || pushes < m_synced || pushes - m_synced >= m_shape.rows) {
      for (std::size_t row = 0; row < m_shape.rows; ++row) {
        fill(row, rowSpan(row));
      }
      upload(0, m_shape.rows);
    } else {
      // Fewer than m_shape.rows rows, newest first.
      for (std::uint64_t k = 0; k < pushes - m_synced; ++k) {
        const std::size_t row = (head + m_shape.rows - static_cast<std::size_t>(k)) % m_shape.rows;
        fill(row, rowSpan(row));
        upload(row, 1);
      }
    }
    m_synced = pushes;
    m_stale = false;
  }
  // Draws every row as `mode`, the oldest first and `head` last, in at most two draw calls.
  void drawOldestFirst(GLenum mode, std::size_t head) const;
  void release();
  void abandon() noexcept;

private:
  void upload(std::size_t firstRow, std::size_t rowCount) const;

  Shape m_shape;
  const char* m_vertexShader;
  const char* m_fragmentShader;
  ShaderProgram m_program;
  GLuint m_placeBuffer = 0;
  GLuint m_valueBuffer = 0;
  GLuint m_indexBuffer = 0;
  GLint m_placeLocation = -1;
  GLint m_valueLocation = -1;
  std::vector<float> m_values;
  std::uint64_t m_synced = 0;
  bool m_stale = true;
};
