#pragma once

#include "render/backend/render_extension.h"
#include "render/core/mat3.h"
#include "render/core/shader_program.h"
#include "workbench/shape_mesh.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class ShapeBatchProgram;

// A vertex buffer of one batch. Only the program that created it creates, fills and deletes the GL buffer, while
// its context is current; a holder only keeps the handle, and dropping it frees the buffer on the program's next
// draw.
class ShapeBatchBuffer {
public:
  // The serial of the program this buffer belongs to.
  std::uint64_t program() const noexcept { return m_program; }

private:
  friend class ShapeBatchProgram;
  std::uint64_t m_program = 0;
  std::uint64_t m_revision = 0; // mesh revision held, 0 for none
  GLuint m_buffer = 0;
};

struct ShapeBatchDraw {
  // Surface size in logical pixels.
  float surfaceWidth = 0;
  float surfaceHeight = 0;
  // Part-local logical pixels to surface logical pixels.
  Mat3 transform = Mat3::identity();
  // Multiplies every shape's alpha.
  float opacity = 1;
};

// The GLES2 program that draws a ShapeMesh (workbench/shape_mesh.h documents the vertex layout and the distance
// functions) with one glDrawArrays call. The output is premultiplied.
class ShapeBatchProgram final : public RenderExtension {
public:
  static constexpr std::size_t kAttributes = 7;

  ShapeBatchProgram();

  // Compiles and links on first use; throws std::runtime_error with the GL log when that fails.
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;

  // A buffer handle for one batch, owned by this program.
  [[nodiscard]] std::shared_ptr<ShapeBatchBuffer> createBuffer();

  // Uploads `vertices` into `buffer` when its revision differs from `revision`, then draws them. Every piece of GL
  // state it changes is restored before it returns. A buffer from another program, an empty or non-finite draw and
  // a program that is not initialized draw nothing.
  void draw(
      ShapeBatchBuffer& buffer, std::span<const luciole::workbench::ShapeVertex> vertices, std::uint64_t revision,
      const ShapeBatchDraw& draw
  );

  // Unique among the programs of the process; a buffer carries the serial of its program.
  std::uint64_t serial() const noexcept { return m_serial; }
  // Vertex uploads done so far.
  std::uint64_t uploads() const noexcept { return m_uploads; }

private:
  void releaseUnused();
  void upload(ShapeBatchBuffer& buffer, std::span<const luciole::workbench::ShapeVertex> vertices);

  std::uint64_t m_serial = 0;
  ShaderProgram m_program;
  std::array<GLint, kAttributes> m_attributes{};
  GLint m_surfaceSizeLocation = -1;
  GLint m_transformLocation = -1;
  GLint m_opacityLocation = -1;
  std::vector<std::shared_ptr<ShapeBatchBuffer>> m_buffers;
  std::uint64_t m_uploads = 0;
};
