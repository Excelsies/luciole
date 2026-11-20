#include "workbench/nest_card.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace luciole::workbench {
  namespace {
    constexpr double kGraze = 1.5;    // how far glyphs may reach over the card's edge
    constexpr double kInkShare = 0.6; // of a glyph's size, the band around its middle that counts

    struct Box {
      double cx = 0; // middle
      double cy = 0;
      double halfW = 0;
      double halfH = 0;
    };

    bool covers(const TreemapRect& card, const Box& box) {
      const auto inner = inset(card, kGraze);
      return box.cx - box.halfW < inner.x + inner.w
          && box.cx + box.halfW > inner.x
          && box.cy - box.halfH < inner.y + inner.h
          && box.cy + box.halfH > inner.y;
    }

    bool hides(const TreemapRect& card, const ShapeLabel& label) {
      const double across = label.fontSize * kInkShare / 2;
      const bool turned = std::abs(std::remainder(label.rotation, 180.0F)) > 45;
      const Box text{
          .cx = label.x + label.width / 2.0,
          .cy = label.y + label.height / 2.0,
          .halfW = turned ? across : label.width / 2.0,
          .halfH = turned ? label.width / 2.0 : across,
      };
      const auto& icon = label.icon;
      const double half = icon.size / 2.0;
      const Box glyph{
          .cx = label.x + icon.left + half,
          .cy = label.y + icon.top + half,
          .halfW = half * kInkShare,
          .halfH = half * kInkShare,
      };
      return covers(card, text) || (icon.size > 0 && covers(card, glyph));
    }
  } // namespace

  SceneShapes nestWithCard(SceneShapes scene, SceneShapes card, const TreemapRect& box) {
    std::vector<NodeId> hidden;
    for (const auto& label : scene.labels) {
      if (hides(box, label)) {
        hidden.push_back(label.node);
      }
    }
    std::erase_if(scene.labels, [&hidden](const ShapeLabel& label) {
      return std::ranges::contains(hidden, label.node);
    });
    scene.shapes.insert(scene.shapes.end(), card.shapes.begin(), card.shapes.end());
    card.labels.insert(card.labels.end(), scene.labels.begin(), scene.labels.end());
    card.labels.resize(std::min(card.labels.size(), kMaxSceneLabels));
    scene.labels = std::move(card.labels);
    return scene;
  }
} // namespace luciole::workbench
