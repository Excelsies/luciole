#pragma once

#include "metrics_renderer.h"
#include "render/scene/input_dispatcher.h"
#include "ui/workbench/column_preview_view.h"

#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>

// A column preview view laid out with a measuring renderer, and the pointer through a real input dispatcher.
namespace test::preview_view {
  struct Rig {
    test::MetricsRenderer renderer;
    std::unique_ptr<luciole::ColumnPreviewView> view = std::make_unique<luciole::ColumnPreviewView>();
    InputDispatcher dispatcher;
    float width = 360;
    float height = 480;
    std::uint32_t serial = 0;

    Rig() { dispatcher.setSceneRoot(view.get()); }

    void layout() { view->arrange(renderer, {0, 0, width, height}); }

    void click(const Node& node) {
      float x = 0;
      float y = 0;
      Node::absolutePosition(&node, x, y);
      x += node.width() / 2;
      y += node.height() / 2;
      dispatcher.pointerEnter(x, y, ++serial);
      dispatcher.pointerButton(x, y, BTN_LEFT, true, ++serial, 0, false);
      dispatcher.pointerButton(x, y, BTN_LEFT, false, ++serial, 0, false);
      layout();
    }
  };
} // namespace test::preview_view
