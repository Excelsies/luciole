#pragma once

#include "render/core/color.h"
#include "workbench/color.h"
#include "workbench/scene_shapes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

class Glyph;
class Node;
class Renderer;
class TextNode;

// The toolkit glyph that stands for a file category.
std::string_view categoryGlyph(luciole::workbench::FileCategory category);

// Width of `text` at `size` px in `weight`.
using LabelMeasure = std::function<double(std::string_view text, double size, luciole::workbench::LabelWeight weight)>;

// Fits each label to its box by its LabelFit: a name is cut as fitNestText cuts it, a count or measure shows its text
// or its short text whole. This is the one place label text is cut. A trail becomes a whole label of its own just
// after its label, only while that label shows, and a line under a count only while the count shows its whole text.
// Labels left without text are removed.
void fitShapeLabels(std::vector<luciole::workbench::ShapeLabel>& labels, const LabelMeasure& measure);

// The text nodes of a shape view's labels, at most kMaxSceneLabels of them, kept as children of one layer node, with
// a glyph node beside each label that has an icon. Texts, sizes and widths are set only when labels are shown or a
// transition begins; during a transition labels only move and fade, so no layout runs. A text is laid out whole, as
// fitShapeLabels left it, its node at most as wide as its box unless the laid-out text is wider, and placed from its
// measured width: a centered label has its ink centered in its box, an end-aligned one ends at the box's right edge.
class ShapeLabels {
public:
  static constexpr std::size_t kCap = luciole::workbench::kMaxSceneLabels;

  explicit ShapeLabels(Node& layer);

  // Shows the first kCap of `labels` at once and hides every other node.
  void show(std::span<const luciole::workbench::ShapeLabel> labels);
  // Starts moving to the first kCap of `labels` from where each label is drawn now, mid-transition included: a
  // label kept with the same node and text moves, others fade out or in. A leaving label whose node a new one needs
  // vanishes at once.
  void beginTransition(std::span<const luciole::workbench::ShapeLabel> labels);
  // Positions and opacities at `progress`, 0 to 1.
  void setProgress(float progress);
  // Ends the transition: labels that left are hidden.
  void finish();
  // Measures the labels and icons shown since the last call, or every one when the render scale changed, widens a
  // node whose laid-out text runs past its box, and places them.
  void place(Renderer& renderer);

private:
  enum class Motion : std::uint8_t { Hidden, Shown, Moving, Leaving };
  // Left edge, middle of the label's box, width, rotation in degrees, color, opacity and the icon's top left.
  struct Pose {
    float x = 0;
    float middle = 0;
    float width = 0;
    float rotation = 0;
    Color color{};
    float opacity = 1;
    float iconX = 0;
    float iconY = 0;
  };
  struct Slot {
    TextNode* node = nullptr;
    Glyph* icon = nullptr; // made the first time the slot shows a label with an icon
    luciole::workbench::NodeId label = luciole::workbench::kNoNode;
    Motion motion = Motion::Hidden;
    Pose from{};
    Pose to{};
    luciole::workbench::LabelAlign align = luciole::workbench::LabelAlign::Start;
    luciole::workbench::LabelIcon iconSpec{};
    float capHeight = 0;
    float textWidth = 0; // advance of the text as drawn
    float inkMiddle = 0; // middle of the ink from the text's origin
    bool measured = false;
  };

  static Pose poseOf(const luciole::workbench::ShapeLabel& label, float opacity);
  static Pose mixed(const Pose& from, const Pose& to, float t);
  Slot* freeSlot();
  void assign(Slot& slot, const luciole::workbench::ShapeLabel& label, Motion motion, const Pose& from);
  void showIcon(Slot& slot);
  void measure(Renderer& renderer, Slot& slot);
  void apply(Slot& slot) const;
  void hide(Slot& slot);

  Node& m_layer;
  std::vector<Slot> m_slots;
  float m_progress = 1;
  float m_measuredScale = 0;
  std::uint64_t m_measuredGeneration = 0;
};
