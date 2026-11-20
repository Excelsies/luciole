#include "ui/controls/shape_batch_node.h"

#include "render/backend/render_backend.h"
#include "ui/controls/shape_batch_program.h"

void ShapeBatchNode::setShapes(std::span<const luciole::workbench::Shape> shapes, float pixelScale) {
  luciole::workbench::buildShapeMesh(shapes, pixelScale, m_mesh);
  m_meshScale = pixelScale;
  ++m_revision;
  markPaintDirty();
}

void ShapeBatchNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  if (m_mesh.vertices.empty()) {
    return;
  }
  auto& program = backend.extension<ShapeBatchProgram>();
  program.ensureInitialized();
  if (!m_buffer || m_buffer->program() != program.serial()) {
    m_buffer = program.createBuffer();
  }
  program.draw(
      *m_buffer, m_mesh.vertices, m_revision,
      ShapeBatchDraw{
          .surfaceWidth = context.surfaceWidth,
          .surfaceHeight = context.surfaceHeight,
          .transform = context.transform,
          .opacity = context.opacity,
      }
  );
}
