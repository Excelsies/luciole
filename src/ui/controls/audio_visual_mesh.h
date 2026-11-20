#pragma once

#include "render/core/color.h"
#include "render/core/shader_program.h"
#include "ui/controls/audio_visual.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>
#include <span>

// Four unsigned bytes per vertex at the attribute a_vertex: a sound visual's vertex shader turns them into a place
// from its uniforms, so its buffer is written once per shape and each frame changes only uniforms.
using AudioVisualVertex = std::array<std::uint8_t, 4>;

// The program and the static vertex buffer one sound visual draws with.
class AudioVisualMesh {
public:
  AudioVisualMesh() = default;
  AudioVisualMesh(const AudioVisualMesh&) = delete;
  AudioVisualMesh& operator=(const AudioVisualMesh&) = delete;

  // Compiles and links on first use; throws std::runtime_error when that fails or the program has no a_vertex.
  void ensureProgram(const char* vertexShader, const char* fragmentShader);
  bool hasProgram() const noexcept { return m_program.isValid(); }
  // The attribute location every draw keeps and restores; -1 without a program.
  GLint vertexLocation() const noexcept { return m_vertex; }
  // The location of a uniform the program uses. When it has none by that name, frees the program and the buffer, so
  // the next setup starts over, and throws std::runtime_error.
  GLint uniform(const char* name);

  // Whether the buffer holds the vertices made for `shape`.
  bool holds(std::uint64_t shape) const noexcept { return m_buffer != 0 && m_shape == shape; }
  // Fills the buffer with the vertices made for `shape`, creating it on first use. Leaves it bound.
  void fill(std::uint64_t shape, std::span<const AudioVisualVertex> vertices);
  // Uses the program and points a_vertex at the buffer.
  void bind() const;
  GLsizei vertexCount() const noexcept { return m_count; }

  void release();
  void abandon() noexcept;

private:
  ShaderProgram m_program;
  GLint m_vertex = -1;
  GLuint m_buffer = 0;
  GLsizei m_count = 0;
  std::uint64_t m_shape = 0;
};

// The host's opacity as a factor from 0 to 1; 0 for a value that is not a number.
float audioVisualOpacity(const AudioVisualDraw& draw);
// Sets a vec3 uniform to a color's red, green and blue.
void setColorUniform(GLint location, const Color& color);
