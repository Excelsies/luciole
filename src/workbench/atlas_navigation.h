#pragma once

#include "workbench/atlas_camera.h"
#include "workbench/pane_layout.h"
#include "workbench/tree.h"

#include <vector>

// The rules that follow every Atlas pan and zoom: entering and leaving folders, and keeping the camera in range.
namespace luciole::workbench {
  // A folder on the line from the view center down: each circle containing the center, nearest level first,
  // with its radius in view pixels.
  struct ProbeLevel {
    NodeId node = kNoNode;
    double r = 0;
  };
  std::vector<ProbeLevel> probe(const AtlasSource& source, NodeId cwd, const Camera& camera);

  // Where the camera rules leave the view: the current folder, the camera in its frame, and each folder
  // entered or left on the way.
  struct CameraState {
    NodeId cwd = kNoNode;
    Camera camera;
    std::vector<NodeId> steps;
  };

  // Promotes the folder under the view center once its diameter reaches 55% of the reference length, and
  // the parent once the current circle's diameter falls under 35%, or, while zooming in, once the view
  // center leaves the current circle. At most 16 promotions.
  CameraState settle(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, bool zoomingIn);

  // Keeps the zoom between 60% of the rest scale at the top and 1.7 reference lengths for the deepest circle
  // under the center, and the center inside the parent's circle; zoom limits keep the point at view offset
  // (mx, my) fixed, as zoomAbout does. A camera that is not finite with a positive scale becomes the rest
  // camera.
  Camera clampCamera(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double mx, double my);

  // settle, then clampCamera: what follows every pan or zoom.
  CameraState afterMove(
      const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, bool zoomingIn, double mx, double my
  );

  // Zooms by `factor` about the view point (px, py), then applies the rules.
  CameraState zoomAt(
      const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double factor, double px, double py
  );

  // True when the cursor's bubble is within 30 px of the edge or under 6 px in radius.
  bool cursorNeedsReveal(const AtlasSource& source, NodeId cwd, NodeId cursor, const Camera& camera, ViewSize view);
} // namespace luciole::workbench
