#include "workbench/iris_overlay_shapes.h"

#include "workbench/iris_hit.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace luciole::workbench::scene_style {
  namespace {
    // Text boxes are measured with the label font's advances; this much room keeps rounding from cutting them.
    constexpr double kTextSlack = 1;
    constexpr float kReadoutFactsGap = 8; // between the readout's name and its facts

    // The box width of a label for text already fitted to `room`: it spans the room, so the label code never cuts
    // the text or drops it as too narrow.
    double labelWidth(double textWidth, double room) { return std::max(textWidth + kTextSlack, room); }

    Rgb inkOf(iris::HubInk ink, const ColorPalette& p) {
      switch (ink) {
      case iris::HubInk::Total:
        return p.primary;
      case iris::HubInk::Items:
        return p.onSurfaceVariant;
      case iris::HubInk::Note:
        return p.secondary;
      case iris::HubInk::Name:
        break;
      }
      return p.onSurface;
    }

    void addHub(Builder& b, const iris::Scene& scene) {
      const auto& hub = scene.hub;
      const auto& p = b.palette;
      // The hub's text outranks every piece's, so it always keeps its labels.
      const double area = scene.dial.width * scene.dial.height;
      const double disc = 2 * hub.radius;
      for (const auto& line : hub.lines) {
        const auto& box = line.box;
        const bool bold = line.ink == iris::HubInk::Name || line.ink == iris::HubInk::Total;
        const double width = labelWidth(box.width, disc);
        auto* label = b.label(
            scene.folder, line.text, box.x + (box.width - width) / 2, box.y + box.height / 2, width, inkOf(line.ink, p),
            area, LabelAlign::Center, line.fontSize
        );
        if (label == nullptr) {
          continue;
        }
        if (bold) {
          label->weight = LabelWeight::Semibold;
        }
        if (line.ink == iris::HubInk::Total || line.ink == iris::HubInk::Items) {
          label->fit = LabelFit::Whole;
        }
      }
      for (std::size_t i = 0; i < hub.chips.size(); ++i) {
        const auto& chip = hub.chips[i];
        const auto& box = chip.box;
        auto shape = rectShape(iris::chipId(i), box.x, box.y, box.width, box.height, box.height / 2);
        // Chips differ only by their part index, which Builder::hovered leaves out.
        const bool hot = b.marks.hovered == shape.id;
        shape.fill = opaque(hot ? p.surfaceContainerHighest : p.surfaceContainerHigh);
        shape.stroke = opaque(hot ? p.secondary : p.outlineVariant);
        shape.strokeWidth = 1;
        b.put(shape);
        b.label(
            chip.node, chip.text, box.x + iris::kChipPad, box.y + box.height / 2,
            labelWidth(box.width - 2 * iris::kChipPad, disc), p.onSurface, area, LabelAlign::Start, chip.fontSize
        );
      }
    }

    void addStack(Builder& b, const iris::AncestorStack& stack) {
      const auto& p = b.palette;
      for (const auto& row : stack.rows) {
        const auto& box = row.box;
        auto shape = rectShape(sid(row.node, SceneRole::Stack), box.x, box.y, box.width, box.height, kIrisStackRadius);
        const bool hot = b.hovered(shape.id);
        shape.fill = tone(p.surfaceContainerHigh, hot ? 1.0 : 0.0);
        shape.opacity = static_cast<float>(row.opacity);
        b.put(shape);
        const auto ink = hot ? p.onSurface : p.onSurfaceVariant;
        auto* label = b.label(
            row.node, row.text, box.x + iris::kStackPad, box.y + box.height / 2,
            labelWidth(box.width - 2 * iris::kStackPad, stack.textWidth), ink, box.width * box.height,
            LabelAlign::Start, stack.fontSize
        );
        if (label != nullptr) {
          label->color = tone(ink, row.opacity);
        }
      }
    }
  } // namespace

  void addIrisOverlay(Builder& b, const iris::Scene& scene, iris::ReadoutLine readout) {
    addHub(b, scene);
    addStack(b, scene.stack);
    if (scene.readout) {
      const auto& box = scene.readout->box;
      const auto ink = b.palette.onSurfaceVariant;
      auto* label = b.label(
          scene.folder, std::move(readout.text), box.x, box.y + box.height / 2, box.width, ink,
          scene.dial.width * scene.dial.height, LabelAlign::Start, scene.readout->fontSize
      );
      if (label == nullptr) {
        return;
      }
      if (readout.facts.empty()) {
        label->fit = LabelFit::Whole; // a count: "N more entries"
        return;
      }
      label->trail = LabelTrail{
          .text = std::move(readout.facts),
          .place = TrailPlace::End,
          .fontSize = static_cast<float>(scene.readout->fontSize),
          .color = opaque(ink),
          .gap = kReadoutFactsGap,
          .room = static_cast<float>(box.width),
      };
    }
  }
} // namespace luciole::workbench::scene_style
