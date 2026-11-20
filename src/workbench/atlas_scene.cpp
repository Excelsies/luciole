#include "workbench/atlas_scene.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <span>
#include <unordered_map>

namespace luciole::workbench {
  double fadeRamp(double value, double lo, double hi) {
    const double t = (value - lo) / (hi - lo);
    return std::isnan(t) ? 0.0 : std::clamp(t, 0.0, 1.0);
  }

  namespace {
    constexpr double kMinDraw = 1.2;
    constexpr double kKidsFrom = 44;
    constexpr double kHugeRim = 3e4;
    constexpr std::size_t kMaxAncestors = 4;
    constexpr double kSiblingFade = 0.6;
    constexpr double kVisibleSlack = 2;

    class SceneBuilder {
    public:
      SceneBuilder(const AtlasSource& source, NodeId cwd, ViewSize view)
          : m_source(source), m_force(pathFromTop(source.tree, cwd)), m_view(view),
            m_limit(2 * source.tree.nodes.size() + 8) {}

      // Emits `root` and, depth first, every visible circle inside it.
      void emitTree(const SceneItem& root, std::vector<SceneItem>& items) {
        std::vector<SceneItem> pending{root};
        while (!pending.empty() && items.size() < m_limit) {
          auto item = pending.back();
          pending.pop_back();
          if (item.r < kMinDraw || !visible(item)) {
            continue;
          }
          const auto& tree = m_source.tree;
          const bool folder = item.role != BubbleRole::More && hasNode(tree, item.node) && tree.nodes[item.node].folder;
          if (!folder || item.r < kKidsFrom) {
            items.push_back(item);
            continue;
          }
          const auto packing = m_source.packings.packing(tree, item.node, m_source.settings, m_force);
          item.empty = packing->items.empty() && !packing->failed;
          item.kids = !packing->items.empty();
          item.labeledKids = item.kids && item.r >= kAtlasKidsLabeledFrom;
          items.push_back(item);
          const auto first = pending.size();
          for (const auto& child : packing->items) {
            pending.push_back(childOf(item, child, *packing));
          }
          std::ranges::reverse(pending.begin() + static_cast<std::ptrdiff_t>(first), pending.end());
        }
      }

      std::span<const NodeId> force() const { return m_force; }

    private:
      bool visible(const SceneItem& item) const {
        return item.x + item.r >= -kVisibleSlack
            && item.x - item.r <= m_view.width + kVisibleSlack
            && item.y + item.r >= -kVisibleSlack
            && item.y - item.r <= m_view.height + kVisibleSlack;
      }

      static SceneItem childOf(const SceneItem& parent, const PackedItem& child, const FolderPacking& packing) {
        const bool siblingLine = parent.role == BubbleRole::Sibling || parent.role == BubbleRole::SiblingChild;
        const auto role = siblingLine ? BubbleRole::SiblingChild
            : parent.depth == 0       ? BubbleRole::Bubble
                                      : BubbleRole::Grandchild;
        const double fade = parent.fade * fadeRamp(parent.r, 39, 49) * (0.42 + 0.58 * fadeRamp(parent.r, 100, 120));
        SceneItem item{
            .node = child.node,
            .folder = parent.node,
            .role = child.more ? BubbleRole::More : role,
            .x = parent.x + child.x * parent.r,
            .y = parent.y + child.y * parent.r,
            .r = child.r * parent.r,
            .fade = fade,
            .depth = parent.depth + 1,
            .clickable = parent.depth == 0 || parent.labeledKids,
        };
        if (child.more) {
          item.restCount = packing.rest.size();
        }
        return item;
      }

      const AtlasSource& m_source;
      std::vector<NodeId> m_force;
      ViewSize m_view;
      std::size_t m_limit;
    };

    struct AncestorFrame {
      NodeId node = kNoNode;
      Similarity frame; // the ancestor's unit circle in cwd units
      std::size_t level = 0;
    };

    std::vector<AncestorFrame> ancestorFrames(const AtlasSource& source, NodeId cwd, std::span<const NodeId> force) {
      std::vector<AncestorFrame> chain;
      Similarity frame;
      auto node = cwd;
      for (std::size_t i = 0; i < kMaxAncestors; ++i) {
        const auto parent = parentOf(source.tree, node);
        const auto p = placementIn(source, parent, node, force);
        if (!p) {
          break;
        }
        frame = {
            .ax = frame.ax - (frame.k * p->x) / p->r, .ay = frame.ay - (frame.k * p->y) / p->r, .k = frame.k / p->r
        };
        node = parent;
        chain.push_back({.node = node, .frame = frame, .level = i + 1});
      }
      return chain;
    }
  } // namespace

  AtlasScene buildScene(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view) {
    AtlasScene scene{.items = {}, .view = view, .camera = camera};
    const bool area = std::isfinite(view.width) && std::isfinite(view.height) && view.width > 0 && view.height > 0;
    if (!area || !usableCamera(camera) || !hasNode(source.tree, cwd)) {
      return scene;
    }
    SceneBuilder builder(source, cwd, view);
    const auto chain = ancestorFrames(source, cwd, builder.force());
    for (const auto& ancestor : chain | std::views::reverse) {
      SceneItem rim{
          .node = ancestor.node,
          .role = BubbleRole::Ancestor,
          .x = view.width / 2 + (ancestor.frame.ax - camera.cx) * camera.scale,
          .y = view.height / 2 + (ancestor.frame.ay - camera.cy) * camera.scale,
          .r = ancestor.frame.k * camera.scale,
          .depth = -static_cast<int>(ancestor.level),
          .level = ancestor.level,
          .clickable = true,
      };
      rim.huge = rim.r > kHugeRim;
      scene.items.push_back(rim);
      if (ancestor.level != 1 || rim.huge) {
        continue;
      }
      const auto packing = source.packings.packing(source.tree, ancestor.node, source.settings, builder.force());
      for (const auto& child : packing->items) {
        if (child.node == cwd) {
          continue;
        }
        builder.emitTree(
            {.node = child.node,
             .folder = ancestor.node,
             .role = child.more ? BubbleRole::More : BubbleRole::Sibling,
             .x = rim.x + child.x * rim.r,
             .y = rim.y + child.y * rim.r,
             .r = child.r * rim.r,
             .fade = kSiblingFade,
             .depth = 1,
             .restCount = child.more ? packing->rest.size() : 0,
             .clickable = true},
            scene.items
        );
      }
    }
    builder.emitTree(
        {.node = cwd,
         .role = BubbleRole::Self,
         .x = view.width / 2 - camera.cx * camera.scale,
         .y = view.height / 2 - camera.cy * camera.scale,
         .r = camera.scale,
         .clickable = true},
        scene.items
    );
    return scene;
  }

  AtlasScene
  buildSceneAround(const AtlasSource& source, NodeId cwd, const Camera& camera, ViewSize view, double margin) {
    const double share = std::isfinite(margin) && margin > 0 ? margin : 0.0;
    const double mx = share * view.width;
    const double my = share * view.height;
    auto scene = buildScene(source, cwd, camera, {.width = view.width + 2 * mx, .height = view.height + 2 * my});
    for (auto& item : scene.items) {
      item.x -= mx;
      item.y -= my;
    }
    scene.view = view;
    return scene;
  }

  std::optional<std::size_t> hitTest(const AtlasScene& scene, double x, double y) {
    for (std::size_t i = scene.items.size(); i-- > 0;) {
      const auto& item = scene.items[i];
      const double dx = x - item.x;
      const double dy = y - item.y;
      if (item.clickable && (item.huge || dx * dx + dy * dy <= item.r * item.r)) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::optional<std::size_t> findFocus(const AtlasScene& scene, const Tree& tree, NodeId node) {
    std::unordered_map<NodeId, std::size_t> shown;
    for (std::size_t i = 0; i < scene.items.size(); ++i) {
      const auto& item = scene.items[i];
      if (item.node != kNoNode && item.role != BubbleRole::Ancestor) {
        shown.try_emplace(item.node, i);
      }
    }
    auto current = node;
    for (std::size_t guard = 0; hasNode(tree, current) && guard <= tree.nodes.size(); ++guard) {
      if (const auto found = shown.find(current); found != shown.end()) {
        if (scene.items[found->second].role != BubbleRole::Self || current == node) {
          return found->second;
        }
        return std::nullopt;
      }
      current = tree.nodes[current].parent;
    }
    return std::nullopt;
  }
} // namespace luciole::workbench
