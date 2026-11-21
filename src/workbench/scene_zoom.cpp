#include "workbench/scene_zoom.h"

#include <cmath>
#include <cstddef>

namespace luciole::workbench {
  // The zoom below names every field it scales. A new field in one of these structs fails the count: scale it in the
  // matching function, or leave it unscaled on purpose, then update the count here.
  namespace zoom_fields {
    struct AnyField {
      template <class T> operator T() const; // NOLINT(google-explicit-constructor)
    };

    // The most members an aggregate takes in an initializer list: its field count.
    template <class T, class... Fields> consteval std::size_t fieldCount() {
      if constexpr (requires { T{Fields{}..., AnyField{}}; }) {
        return fieldCount<T, Fields..., AnyField>();
      } else {
        return sizeof...(Fields);
      }
    }
  } // namespace zoom_fields
  static_assert(zoom_fields::fieldCount<Shape>() == 16, "zoomShape lists the fields of Shape");
  static_assert(zoom_fields::fieldCount<ShapeLabel>() == 15, "zoomLabel lists the fields of ShapeLabel");
  static_assert(zoom_fields::fieldCount<LabelTrail>() == 9, "zoomLabel lists the fields of LabelTrail");

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
