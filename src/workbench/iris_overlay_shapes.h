#pragma once

#include "workbench/iris_facts.h"
#include "workbench/iris_scene.h"
#include "workbench/scene_style.h"

// The text around the ring diagram, drawn into a scene's builder; private to the scene shape builders.
namespace luciole::workbench::scene_style {
  // The hub's lines and trail chips, the ancestor stack, and `readout` in the readout line of a compact dial: the name
  // cut to fit, its facts a whole trail at the line's right end, and a message with no facts whole.
  void addIrisOverlay(Builder& b, const iris::Scene& scene, iris::ReadoutLine readout);
} // namespace luciole::workbench::scene_style
