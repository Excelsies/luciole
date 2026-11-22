#pragma once

#include "render/scene/custom_draw_node.h"
#include "workbench/shape.h"
#include "workbench/shape_mesh.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

class ShapeBatchBuffer;

// Draws a shape list with one draw call under the node's transform, opacity and clip. The mesh is built when the
// shapes are set and uploaded on the first draw after it changes.
class ShapeBatchNode final : public CustomDrawNode {
public:
  // Builds the mesh of `shapes` with edges antialiased for `pixelScale` buffer pixels per logical pixel, and marks
  // the node for repaint. A scale that is not positive and finite gives an empty mesh.
  void setShapes(std::span<const luciole::workbench::Shape> shapes, float pixelScale);

  const luciole::workbench::ShapeMesh& mesh() const noexcept { return m_mesh; }
  float meshScale() const noexcept { return m_meshScale; }

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  // Vertices a buffer may hold beyond four times the scene's before it is shrunk.
  static constexpr std::size_t kKeptVertices = 4096;

  luciole::workbench::ShapeMesh m_mesh;
  float m_meshScale = 0;
  std::uint64_t m_revision = 0;
  mutable std::shared_ptr<ShapeBatchBuffer> m_buffer;
};
