#include "workbench/scene_zoom.h"

#include <cmath>

namespace luciole::workbench {
  namespace {
    void zoomPoint(ShapePoint& point, float f) {
      point.x *= f;
      point.y *= f;
    }

    void zoomShape(Shape& shape, float f) {
      shape.rect.x *= f;
      shape.rect.y *= f;
      shape.rect.width *= f;
      shape.rect.height *= f;
      shape.rect.radius *= f;
      zoomPoint(shape.sector.center, f);
      shape.sector.r0 *= f;
      shape.sector.r1 *= f;
      zoomPoint(shape.circle.center, f);
      shape.circle.radius *= f;
      for (auto& point : shape.ribbon.top) {
        zoomPoint(point, f);
      }
      for (auto& point : shape.ribbon.bottom) {
        zoomPoint(point, f);
      }
      zoomPoint(shape.line.from, f);
      zoomPoint(shape.line.to, f);
      shape.line.width *= f;
    }

    void zoomLabel(ShapeLabel& label, float f) {
      label.x *= f;
      label.y *= f;
      label.width *= f;
      label.height *= f;
      label.icon.size *= f;
      label.icon.left *= f;
      label.icon.top *= f;
      if (label.trail) {
        label.trail->gap *= f;
        label.trail->room *= f;
        label.trail->dx *= f;
        label.trail->dy *= f;
        label.trail->width *= f;
      }
    }
  } // namespace

  void zoomScene(SceneShapes& scene, double factor) {
    if (!(factor > 0) || !std::isfinite(factor) || factor == 1.0) {
      return;
    }
    const auto f = static_cast<float>(factor);
    for (auto& shape : scene.shapes) {
      zoomShape(shape, f);
    }
    for (auto& label : scene.labels) {
      zoomLabel(label, f);
    }
  }
} // namespace luciole::workbench
