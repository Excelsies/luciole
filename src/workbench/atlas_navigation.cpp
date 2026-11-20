#include "workbench/atlas_navigation.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace luciole::workbench {
  namespace {
    constexpr std::size_t kMaxProbe = 60;
    constexpr std::size_t kMaxPromotions = 16;
    constexpr double kDive = 0.55;
    constexpr double kSurface = 0.35;
    constexpr double kRootMin = 0.6;
    constexpr double kMaxFill = 1.7;
    constexpr double kRootBox = 1.1;
    constexpr double kParentBox = 0.97;
    constexpr double kRevealMargin = 30;
    constexpr double kRevealLeast = 6;

    bool isFolder(const Tree& tree, NodeId id) { return hasNode(tree, id) && tree.nodes[id].folder; }

    // Where the current folder sits in its parent; empty at the top.
    std::optional<PackedItem> placementInParent(const AtlasSource& source, NodeId cwd) {
      const auto force = pathFromTop(source.tree, cwd);
      return placementIn(source, parentOf(source.tree, cwd), cwd, force);
    }
  } // namespace

  std::vector<ProbeLevel> probe(const AtlasSource& source, NodeId cwd, const Camera& camera) {
    std::vector<ProbeLevel> chain;
    if (!usableCamera(camera)) {
      return chain;
    }
    const auto force = pathFromTop(source.tree, cwd);
    double px = camera.cx;
    double py = camera.cy;
    double r = camera.scale;
    for (auto node = cwd; chain.size() < kMaxProbe && isFolder(source.tree, node);) {
      const auto packing = source.packings.packing(source.tree, node, source.settings, force);
      const auto hit = std::ranges::find_if(packing->items, [&](const PackedItem& item) {
        return !item.more && (px - item.x) * (px - item.x) + (py - item.y) * (py - item.y) <= item.r * item.r;
      });
      if (hit == packing->items.end()) {
        break;
      }
      px = (px - hit->x) / hit->r;
      py = (py - hit->y) / hit->r;
      r *= hit->r;
      chain.push_back({.node = hit->node, .r = r});
      node = hit->node;
    }
    return chain;
  }

  CameraState settle(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, bool zoomingIn) {
    CameraState state{.cwd = cwd, .camera = camera, .steps = {}};
    if (!usableCamera(camera) || !hasNode(source.tree, cwd)) {
      return state;
    }
    const double reference = referenceLength(view);
    for (std::size_t step = 0; step < kMaxPromotions; ++step) {
      const auto& cam = state.camera;
      const bool inParent = placementInParent(source, state.cwd).has_value();
      const bool small = 2 * cam.scale < kSurface * reference;
      const bool outside = zoomingIn && cam.cx * cam.cx + cam.cy * cam.cy > 1;
      std::optional<NodeId> target;
      if (inParent && (small || outside)) {
        target = parentOf(source.tree, state.cwd);
      } else if (zoomingIn) {
        const auto levels = probe(source, state.cwd, cam);
        if (!levels.empty()
            && isFolder(source.tree, levels.front().node)
            && 2 * levels.front().r >= kDive * reference) {
          target = levels.front().node;
        }
      }
      const auto sim = target ? similarity(source, state.cwd, *target) : std::nullopt;
      if (!sim) {
        break;
      }
      state.camera = convert(state.camera, *sim);
      state.cwd = *target;
      state.steps.push_back(*target);
    }
    return state;
  }

  Camera clampCamera(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double mx, double my) {
    if (!usableCamera(camera)) {
      return restCamera(view);
    }
    const double reference = referenceLength(view);
    const auto inParent = placementInParent(source, cwd);
    auto cam = camera;
    const double floor = restScale(view) * kRootMin;
    if (!inParent && cam.scale < floor) {
      cam = zoomAbout(cam, floor / cam.scale, mx, my);
    }
    const auto levels = probe(source, cwd, cam);
    const double deepest = levels.empty() ? cam.scale : levels.back().r;
    if (deepest > kMaxFill * reference) {
      cam = zoomAbout(cam, (kMaxFill * reference) / deepest, mx, my);
    }
    const Circle box = inParent
        ? Circle{.x = -inParent->x / inParent->r, .y = -inParent->y / inParent->r, .r = kParentBox / inParent->r}
        : Circle{.x = 0, .y = 0, .r = kRootBox};
    const double dx = cam.cx - box.x;
    const double dy = cam.cy - box.y;
    const double distance = std::hypot(dx, dy);
    if (distance > box.r) {
      cam.cx = box.x + (dx / distance) * box.r;
      cam.cy = box.y + (dy / distance) * box.r;
    }
    return cam;
  }

  CameraState afterMove(
      const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, bool zoomingIn, double mx, double my
  ) {
    auto state = settle(source, cwd, camera, view, zoomingIn);
    state.camera = clampCamera(source, state.cwd, state.camera, view, mx, my);
    return state;
  }

  CameraState zoomAt(
      const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double factor, double px, double py
  ) {
    const double mx = px - view.width / 2;
    const double my = py - view.height / 2;
    return afterMove(source, cwd, zoomAbout(camera, factor, mx, my), view, factor > 1, mx, my);
  }

  bool cursorNeedsReveal(const AtlasSource& source, NodeId cwd, NodeId cursor, const Camera& camera, ViewSize view) {
    if (!hasNode(source.tree, cwd)) {
      return false;
    }
    const auto force = pathFromTop(source.tree, cwd);
    const auto item = source.packings.packing(source.tree, cwd, source.settings, force)->placement(cursor);
    if (!item) {
      return false;
    }
    const double x = view.width / 2 + (item->x - camera.cx) * camera.scale;
    const double y = view.height / 2 + (item->y - camera.cy) * camera.scale;
    return x < kRevealMargin
        || y < kRevealMargin
        || x > view.width - kRevealMargin
        || y > view.height - kRevealMargin
        || item->r * camera.scale < kRevealLeast;
  }
} // namespace luciole::workbench
