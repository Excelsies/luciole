#include "ui/workbench/atlas_picture.h"

#include "ui/workbench/scene_part.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace luciole {
  namespace {
    using workbench::SceneRole;
    using workbench::Shape;

    Shape cover(workbench::ShapeId id, const workbench::Rect& map) {
      auto shape = workbench::rectShape(id, map.x, map.y, map.width, map.height);
      shape.strokeWidth = 0;
      return shape;
    }
  } // namespace

  workbench::ShapeId atlasBackgroundId() {
    return {.node = workbench::kNoNode, .role = workbench::shapeRole(SceneRole::Hub)};
  }

  workbench::ShapeId atlasBarId() {
    return {.node = workbench::kNoNode, .role = workbench::shapeRole(SceneRole::Chip)};
  }

  AtlasHud atlasHud(const workbench::Tree& tree, workbench::ViewSize view, const workbench::LabelFont& font) {
    auto layout = workbench::hudLayout(view, tree.ancestors.size());
    auto placed = workbench::placeHud(layout, workbench::hudExtents(tree, layout, font));
    return {.layout = std::move(layout), .placed = std::move(placed)};
  }

  bool atlasControlAt(const AtlasHud& hud, const workbench::Rect& map, double x, double y) {
    const auto on = [x, y](const workbench::Rect& rect) {
      return x >= rect.x && x <= rect.x + rect.width && y >= rect.y && y <= rect.y + rect.height;
    };
    return y < map.y || std::ranges::any_of(hud.placed, on) || std::ranges::any_of(hud.layout.buttons, on);
  }

  workbench::SceneShapes
  atlasMapLayer(workbench::SceneShapes scene, const workbench::Reframe& frame, const workbench::Rect& map) {
    workbench::reframeShapes(scene, frame);
    offsetScene(scene, static_cast<float>(map.x), static_cast<float>(map.y));
    const auto top = static_cast<float>(map.y);
    std::erase_if(scene.labels, [top](const workbench::ShapeLabel& label) { return !(label.y >= top); });
    return scene;
  }

  workbench::SceneShapes atlasPicture(
      workbench::SceneShapes map, const workbench::SceneShapes* fading, double fadeOpacity, const workbench::Rect& area,
      const workbench::ColorPalette& palette
  ) {
    const double alpha = std::isfinite(fadeOpacity) ? std::clamp(fadeOpacity, 0.0, 1.0) : 0.0;
    if (fading == nullptr || !(alpha > 0)) {
      return map;
    }
    // Text draws over every shape, so the map's labels fade in as the old picture fades out over them.
    for (auto& label : map.labels) {
      label.color.a = static_cast<std::uint8_t>(std::lround(label.color.a * (1 - alpha)));
      label.icon.color.a = static_cast<std::uint8_t>(std::lround(label.icon.color.a * (1 - alpha)));
    }
    map.shapes.reserve(map.shapes.size() + 1 + fading->shapes.size());
    auto backing = cover({.node = workbench::kNoNode, .role = workbench::shapeRole(SceneRole::Detail)}, area);
    backing.fill = {.r = palette.surface.r, .g = palette.surface.g, .b = palette.surface.b, .a = 255};
    backing.opacity = static_cast<float>(alpha);
    backing.hittable = false;
    map.shapes.push_back(backing);
    for (auto shape : fading->shapes) {
      shape.opacity *= static_cast<float>(alpha);
      shape.hittable = false;
      map.shapes.push_back(shape);
    }
    for (auto label : fading->labels) {
      label.color.a = static_cast<std::uint8_t>(std::lround(label.color.a * alpha));
      label.icon.color.a = static_cast<std::uint8_t>(std::lround(label.icon.color.a * alpha));
      map.labels.push_back(std::move(label));
    }
    return map;
  }

  workbench::SceneShapes
  composeAtlas(AtlasLayers layers, const workbench::Rect& map, const workbench::ColorPalette& palette) {
    auto picture = atlasPicture(std::move(layers.map), layers.fading, layers.fadeOpacity, map, palette);
    workbench::SceneShapes out;
    out.shapes.reserve(2 + picture.shapes.size() + layers.hud.shapes.size());
    auto background = cover(atlasBackgroundId(), map);
    background.fill.a = 0;
    out.shapes.push_back(background);
    out.shapes.append_range(std::move(picture.shapes));
    out.labels = std::move(layers.hud.labels);
    out.labels.append_range(std::move(picture.labels));
    if (map.y > 0) {
      auto bar = workbench::rectShape(atlasBarId(), map.x, 0, map.width, map.y);
      bar.strokeWidth = 0;
      bar.fill = {.r = palette.surface.r, .g = palette.surface.g, .b = palette.surface.b, .a = 255};
      out.shapes.push_back(bar);
    }
    out.shapes.append_range(std::move(layers.hud.shapes));
    return out;
  }
} // namespace luciole
