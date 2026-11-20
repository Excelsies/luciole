#pragma once

#include "render/backend/render_extension.h"
#include "render/core/color.h"
#include "render/core/shader_program.h"
#include "render/core/texture_handle.h"
#include "thumbnails/mesh_preview.h"

#include <GLES2/gl2.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class MeshProgram;

// A 4x4 matrix in column-major order.
using MeshMatrix = std::array<float, 16>;

// Directions toward the two lights, in view space: x right, y up, z toward the viewer. The shader normalizes them.
inline constexpr std::array<float, 3> kMeshKeyLight{-0.45F, 0.65F, 0.61F};
inline constexpr std::array<float, 3> kMeshFillLight{0.80F, -0.20F, 0.57F};

struct MeshCamera {
  MeshMatrix modelView{};
  MeshMatrix projection{};
  bool operator==(const MeshCamera&) const = default;
};

// What one drawing shows: the camera and the two light colors.
struct MeshLook {
  MeshCamera camera;
  Color keyLight;
  Color fillLight;
  bool operator==(const MeshLook&) const = default;
};

// A part of a mesh small enough for 16-bit indices.
struct MeshShortPart {
  std::vector<luciole::MeshVertex> vertices;
  std::vector<std::uint16_t> indices;
};

// Splits the triangles, in order, into parts of at most `maxVertices` vertices (at least 3, at most 65536). A vertex
// is copied into every part that uses it.
std::vector<MeshShortPart> splitShortIndices(const luciole::MeshGeometry& geometry, std::size_t maxVertices = 65536);

// The GPU copy of one mesh and the image it is drawn into. Only the program that created it creates and fills its
// objects, while the program's context is current. A holder that drops it leaves its objects to the program's next
// render, or to the program's teardown.
class MeshGpu {
public:
  // The serial of the program this belongs to.
  std::uint64_t program() const noexcept { return m_program; }
  // The drawn image, top row first and premultiplied; empty before the first render.
  TextureId image() const noexcept { return TextureId(m_image); }
  int imageWidth() const noexcept { return m_width; }
  int imageHeight() const noexcept { return m_height; }
  // Images drawn and geometry uploads done so far.
  std::uint64_t renders() const noexcept { return m_renders; }
  std::uint64_t uploads() const noexcept { return m_uploads; }
  // GPU memory held: vertex and index buffers, the image and its depth buffer.
  std::size_t bytes() const noexcept;

  // Deletes every GL object now; the next render makes them again. The program's context must be current.
  void release();

private:
  friend class MeshProgram;
  struct Part {
    GLuint vertices = 0;
    GLuint indices = 0;
    GLsizei count = 0;
    GLenum type = GL_UNSIGNED_INT;
  };
  void forget() noexcept;

  std::uint64_t m_program = 0;
  std::uint64_t m_revision = 0; // geometry revision uploaded, 0 for none
  std::vector<Part> m_parts;
  std::size_t m_bufferBytes = 0;
  GLuint m_image = 0;
  GLuint m_depth = 0;
  int m_width = 0;
  int m_height = 0;
  std::optional<MeshLook> m_drawn; // look of the image, none when it must be drawn
  std::uint64_t m_drawnRevision = 0;
  std::uint64_t m_renders = 0;
  std::uint64_t m_uploads = 0;
};

// The GLES2 program that draws a MeshGeometry with depth testing and two lights. Flat geometry is lit per triangle;
// faces seen from behind are lit like their front. Uses 32-bit indices where the driver offers them and parts with
// 16-bit indices elsewhere.
class MeshProgram final : public RenderExtension {
public:
  static constexpr int kMaxImageSide = 4096;

  MeshProgram();

  // Compiles and links on first use; throws std::runtime_error with the GL log when that fails.
  void ensureInitialized();
  void destroy() override;
  void abandon() noexcept override;

  [[nodiscard]] std::shared_ptr<MeshGpu> createGpu();

  // Uploads `geometry` into `gpu` when `revision` differs from the one it holds, then draws it into the gpu's image
  // of `width` by `height` pixels, each clamped to the largest image the driver allows and kMaxImageSide, on a
  // transparent background, unless the image already shows this revision, look and size. Restores every piece of GL
  // state it changes. False when nothing could be drawn, as for a gpu from another program or a program that is not
  // initialized.
  bool render(
      MeshGpu& gpu, const luciole::MeshGeometry& geometry, std::uint64_t revision, const MeshLook& look, int width,
      int height
  );

  std::uint64_t serial() const noexcept { return m_serial; }
  bool uintIndices() const noexcept { return m_uintIndices; }

private:
  void releaseUnused();
  void upload(MeshGpu& gpu, const luciole::MeshGeometry& geometry);
  bool prepareImage(MeshGpu& gpu, int width, int height);
  void drawParts(const MeshGpu& gpu, const MeshLook& look);

  std::uint64_t m_serial = 0;
  ShaderProgram m_program;
  GLint m_position = -1;
  GLint m_normal = -1;
  GLint m_modelView = -1;
  GLint m_projection = -1;
  GLint m_key = -1;
  GLint m_fill = -1;
  bool m_uintIndices = false;
  int m_maxSide = 0;
  std::vector<std::shared_ptr<MeshGpu>> m_gpus;
};
