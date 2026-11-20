#pragma once

#include "render/scene/custom_draw_node.h"
#include "render/scene/input_area.h"
#include "thumbnails/mesh_preview.h"
#include "ui/controls/mesh_program.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <memory>

class Button;
class Renderer;

// How the camera circles a model: a turn around the up axis and a tilt toward it, in radians, and a zoom factor.
struct MeshOrbit {
  static constexpr float kMaxPitch = 1.5F;
  static constexpr float kMinZoom = 0.25F;
  static constexpr float kMaxZoom = 8.0F;

  float yaw = 0.6F;
  float pitch = 0.45F;
  float zoom = 1.0F;
  bool operator==(const MeshOrbit&) const = default;
};

// The narrowest view, width over height, the camera fits; the widest is its inverse.
inline constexpr float kMinMeshAspect = 1e-4F;

// A perspective camera that fits the sphere around the geometry's bounds into a viewport of `aspect` width over
// height, with the format's up axis pointing up, then turns and zooms by `orbit`. A pitch beyond kMaxPitch, a
// zoom outside kMinZoom to kMaxZoom and an aspect outside kMinMeshAspect to its inverse are clamped; a non-finite
// yaw or pitch counts as zero and a non-finite zoom as 1; an aspect that is not positive and finite counts as 1; an
// empty or single-point box is drawn as a unit sphere around its center. Every entry is finite for bounds within
// kMaxMeshCoordinate.
MeshCamera meshCamera(const luciole::MeshGeometry& geometry, const MeshOrbit& orbit, float aspect);

// A model the user can turn. MeshProgram draws it into an image of the view's size in buffer pixels, lit by the
// palette's primary and tertiary colors on a transparent background, again only when the camera, the size, the
// scale or the palette changes; every other frame shows the image as it is. Dragging with the left button turns
// the model, the wheel zooms, and the button in the top right corner returns to the first view.
class MeshView final : public CustomDrawNode {
public:
  // Radians the model turns per logical pixel dragged, and the zoom exponent per logical pixel of wheel travel.
  static constexpr float kTurnPerPixel = 0.01F;
  static constexpr float kZoomPerPixel = 1.0F / 560.0F;

  MeshView();
  ~MeshView() override;
  MeshView(const MeshView&) = delete;
  MeshView& operator=(const MeshView&) = delete;

  // Shows `geometry` from the first view; empty shows nothing.
  void setGeometry(std::shared_ptr<const luciole::MeshGeometry> geometry);
  const std::shared_ptr<const luciole::MeshGeometry>& geometry() const noexcept { return m_geometry; }

  const MeshOrbit& orbit() const noexcept { return m_orbit; }
  // Returns to the first view.
  void resetOrbit();
  // Brings the camera `scale` times nearer on top of the orbit's zoom, within the zoom's range: 1 keeps the fitted
  // view. A scale that is not finite and positive is ignored.
  void setPreviewScale(float scale);
  float previewScale() const noexcept { return m_previewScale; }
  // The camera the view draws through for a viewport of `aspect` width over height; empty geometry draws nothing.
  MeshCamera camera(float aspect) const;

  // Frees the image, its depth buffer and the vertex and index buffers now; the next draw makes them again.
  // Owners call it when they hide the view. The view also frees them when destroyed, through the renderer of its
  // last layout.
  void release(Renderer& renderer);

  // Images drawn so far, and GPU bytes held.
  std::uint64_t renders() const noexcept;
  std::size_t gpuBytes() const noexcept;
  // The GPU copy the view draws through, without keeping it; empty before the first draw.
  std::weak_ptr<const MeshGpu> gpu() const noexcept { return m_gpu; }

  InputArea& inputArea() const noexcept { return *m_area; }
  Button& resetButton() const noexcept { return *m_reset; }

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

protected:
  void doLayout(Renderer& renderer) override;

private:
  void setOrbit(const MeshOrbit& orbit);
  void press(const InputArea::PointerData& pointer);
  void motion(const InputArea::PointerData& pointer);

  std::shared_ptr<const luciole::MeshGeometry> m_geometry;
  std::uint64_t m_revision = 0;
  MeshOrbit m_orbit;
  float m_previewScale = 1;
  InputArea* m_area = nullptr;
  Button* m_reset = nullptr;
  bool m_dragging = false;
  float m_dragX = 0;
  float m_dragY = 0;
  Renderer* m_renderer = nullptr;
  mutable std::shared_ptr<MeshGpu> m_gpu;
  Signal<>::ScopedConnection m_paletteConn;
};
