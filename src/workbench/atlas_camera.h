#pragma once

#include "workbench/circle_pack.h"
#include "workbench/measure.h"
#include "workbench/pane_layout.h"
#include "workbench/point.h"
#include "workbench/tree.h"

#include <cstddef>
#include <optional>
#include <span>

// The Atlas camera and the frames that relate one folder's circle to another's.
namespace luciole::workbench {
  // View center in the current folder's own units, and pixels per unit.
  struct Camera {
    double cx = 0;
    double cy = 0;
    double scale = 1;
    bool operator==(const Camera&) const = default;
  };

  // Maps points of one frame into another: p_to = (ax, ay) + k * p_from.
  struct Similarity {
    double ax = 0;
    double ay = 0;
    double k = 1;
  };

  // What every Atlas layout reads: the tree, the settings and the packings it may reuse.
  struct AtlasSource {
    const Tree& tree;
    const LayoutSettings& settings;
    PackingCache& packings;
  };

  // Zooms by `factor` keeping the point at view offset (mx, my) from the view center fixed; an offset that is
  // not finite reads as 0. A factor or camera that is not positive and finite leaves the camera as it is.
  Camera zoomAbout(const Camera& camera, double factor, double mx, double my);

  // The same picture in another frame.
  Camera convert(const Camera& camera, const Similarity& sim);

  // Fly path: zoom on a log scale, pan so motion on screen is uniform. u is clamped to 0..1, NaN reads as 0;
  // a camera whose scale is not positive and finite jumps to `to`.
  Camera interpolate(const Camera& from, const Camera& to, double u);

  // Index of the point to move to from `current` in direction (dx, dy): the best within a cone, else the
  // closest one ahead.
  std::optional<std::size_t> nearest(std::span<const Point> points, std::size_t current, double dx, double dy);

  // Tree steps between two nodes; 0 when either is unknown.
  std::size_t treeDistance(const Tree& tree, NodeId from, NodeId to);

  // Maps `from`'s frame into `to`'s through their placements; empty when a placement is missing.
  std::optional<Similarity> similarity(const AtlasSource& source, NodeId from, NodeId to);

  // The camera after the current folder changed from `from` to `to`, when they are at most two steps apart.
  std::optional<Camera> convertCamera(const AtlasSource& source, NodeId from, NodeId to, const Camera& camera);

  // The one reference length for the rest view and every threshold: the smaller side, at least 1.
  double referenceLength(ViewSize view);
  // Pixels per unit at rest: half the smaller side less 14 px, at least 40.
  double restScale(ViewSize view);
  Camera restCamera(ViewSize view);

  // A center that is finite and a scale that is positive and finite.
  bool usableCamera(const Camera& camera);

  // Where `child` sits in `parent`'s packing, keeping the folders in `force`; empty for an unknown parent or a
  // child the packing does not place.
  std::optional<PackedItem>
  placementIn(const AtlasSource& source, NodeId parent, NodeId child, std::span<const NodeId> force);
} // namespace luciole::workbench
