#pragma once

#include "render/scene/input_area.h"
#include "ui/controls/shape_view.h"
#include "workbench/color.h"
#include "workbench/flame_row.h"
#include "workbench/measure.h"
#include "workbench/scene_shapes.h"
#include "workbench/tree.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Renderer;

// What the instrument and the ribbon share: their inputs, the requests they make of their owner, and the scene part
// each kind implements.
namespace luciole {
  // One immutable tree snapshot; the serial changes with every new snapshot.
  struct PartSnapshot {
    std::shared_ptr<const workbench::Tree> tree; // null while the first snapshot loads
    std::uint64_t serial = 0;
  };

  // The folders below the current one that the page last went down through, nearest first, as nodes of the snapshot
  // the part is given. It ends at the first one the snapshot does not hold.
  struct PartTrail {
    std::vector<workbench::NodeId> folders;
    bool operator==(const PartTrail&) const = default;
  };

  // What a part draws, apart from its kind and size.
  struct PartInputs {
    PartSnapshot snapshot;
    workbench::LayoutSettings settings;
    workbench::ColorPalette palette; // the palette's colors, as the scene builders take them
    PartTrail trail{};
  };

  bool samePalette(const workbench::ColorPalette& left, const workbench::ColorPalette& right);

  enum class PartRequestKind : std::uint8_t {
    Go,       // enter the folder
    Select,   // put the cursor on the entry
    Open,     // open the file
    ShowMore, // list the entries a merged piece of the folder holds
    Context,  // a context menu for the entry; kNoNode for the view's background
  };

  // Node ids belong to the view's shownSnapshot().
  struct PartRequest {
    PartRequestKind kind = PartRequestKind::Go;
    workbench::NodeId node = workbench::kNoNode;
    float x = 0; // the pointer in the view at the press; for a key, the piece it acts on when anchored, else 0
    float y = 0;
    std::uint32_t button = 0; // 0 for a key or the wheel
    std::uint32_t modifiers = 0;
    bool anchored = false; // a key's request: the part placed x and y itself
  };

  // Advance widths of the label font at kBaseSize, regular and semibold, measured once by the renderer a view is laid
  // out with. Other sizes scale linearly.
  class LabelMetrics {
  public:
    static constexpr double kBaseSize = 12;

    void measure(Renderer& renderer);
    bool ready() const noexcept { return m_ready; }
    double
    width(std::string_view text, double size, workbench::LabelWeight weight = workbench::LabelWeight::Regular) const;
    workbench::LabelFont font(double size, workbench::LabelWeight weight = workbench::LabelWeight::Regular) const;
    // The same advances at `factor` times the size, for a scene laid out in a view scaled by 1 / factor.
    LabelMetrics scaled(double factor) const;

  private:
    workbench::LabelFont m_font;
    workbench::LabelFont m_semibold;
    bool m_ready = false;
  };

  // A part's inputs for one layout.
  struct PartFrame {
    std::shared_ptr<const workbench::Tree> tree; // never null
    std::uint64_t serial = 0;
    workbench::LayoutSettings settings;
    workbench::ColorPalette palette;
    double width = 0;
    double height = 0;
    LabelMetrics metrics;
    float textScale = 1;            // the system font's scale the scene is laid out for, in view size over `width`
    std::uint64_t metricsEpoch = 0; // changes whenever `metrics` is measured again
    PartTrail trail{};
  };

  // What a wheel turn or a key did.
  struct PartReaction {
    bool handled = false;
    bool relayout = false;             // the part's own view changed: lay it out again at the next update
    std::vector<PartRequest> requests; // in the order the owner carries them out
  };

  // One kind of scene: its layout, its shapes, and what its wheel and keys do.
  class ScenePart {
  public:
    ScenePart() = default;
    ScenePart(const ScenePart&) = delete;
    ScenePart& operator=(const ScenePart&) = delete;
    ScenePart(ScenePart&&) = delete;
    ScenePart& operator=(ScenePart&&) = delete;
    virtual ~ScenePart() = default;

    // Lays the scene out for `frame`. False when the layout finishes on a worker, which then reports it through the
    // ready callback the part was made with.
    virtual bool layout(const PartFrame& frame) = 0;
    // True when a layout that finishes on a worker leaves shapes() drawing the earlier scene as it was laid out, until
    // the reply; false when shapes() already draws at the size of the frame given.
    virtual bool holdsSceneUntilReply() const { return false; }
    // For such a part, the text scale of the frame the scene shapes() draws was laid out for; none before the first.
    virtual std::optional<float> shownTextScale() const { return std::nullopt; }
    // Shapes of the last finished layout with `marks`, in view coordinates; empty before the first.
    virtual workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const = 0;
    // The snapshot of the last finished layout, whose node ids the shapes, the wheel and the keys use; empty before
    // the first.
    virtual PartSnapshot shownSnapshot() const = 0;
    // Where the note over a current folder with no visible entries goes in a view `width` by `height`; none when the
    // scene says it itself or draws nothing of that folder.
    virtual std::optional<workbench::ShapeRect> noteBox(float width, float height) const {
      return workbench::ShapeRect{.x = 0, .y = 0, .width = width, .height = height, .radius = 0};
    }
    virtual PartReaction wheel(const ShapeWheel& /*wheel*/) { return {}; }
    virtual PartReaction key(const InputArea::KeyData& /*key*/, const workbench::ShapeMarks& /*marks*/) { return {}; }
    // A drag over the view. A handled Begin takes the press from the shape under it, and after each handled step the
    // view shows the part's shapes again.
    virtual PartReaction drag(const ShapeDrag& /*drag*/) { return {}; }
    // A click on a shape the part answers itself; an unhandled one becomes the request clickRequest gives.
    virtual PartReaction activate(workbench::ShapeId /*id*/) { return {}; }
    virtual TooltipContent tooltip(workbench::ShapeId /*id*/) const { return {}; }
    // Gives the part its owner's hooks, once, before the first layout: `ready` reports a layout finished on a worker,
    // `redraw` asks for the shapes again, tweening for `transitionMs` once no transition runs.
    virtual void
    connect(const std::function<void()>& /*ready*/, const std::function<void(float transitionMs)>& /*redraw*/) {}
    // The shape under the pointer changed; none when the pointer left the view.
    virtual void hover(std::optional<workbench::ShapeId> /*id*/) {}
    // The entry the pointer over `id` points at in the other panes: hoveredNode unless the part knows better.
    virtual workbench::NodeId pointedNode(workbench::ShapeId id) const;
    // True while the part moves on its own. The view then calls advance once per update with the time of that update,
    // shows the part's shapes again and asks for another frame.
    virtual bool moving() const { return false; }
    virtual PartReaction advance(std::chrono::steady_clock::time_point /*now*/) { return {}; }
    // Length of the transition to a new snapshot or from another kind.
    virtual float transitionMs() const = 0;
  };

  // What a click on a shape asks for: a merged piece lists its folder's entries, the hub goes up, a folder is
  // entered and a file selected. None for the current folder's own pieces and shapes that stand for no entry.
  std::optional<PartRequest> clickRequest(const workbench::Tree& tree, workbench::ShapeId id);

  // The entry a hovered shape points at; kNoNode for merged pieces, quiet bars and buttons such as the hub.
  workbench::NodeId hoveredNode(workbench::ShapeId id);

  // Enter on the cursor's entry: Go for a folder, Open for a file; none for an unknown node.
  std::optional<PartRequest> enterRequest(const workbench::Tree& tree, workbench::NodeId cursor);

  // Backspace: Go to the current folder's parent; none at the top of the snapshot.
  std::optional<PartRequest> upRequest(const workbench::Tree& tree);

  // Unit direction of an arrow key; none for other keys.
  struct KeyStep {
    double dx = 0;
    double dy = 0;
  };
  std::optional<KeyStep> arrowStep(std::uint32_t sym);

  // The wheel of Nest and Iris: up over a folder of the current folder, or of one of its folders, enters it, down
  // goes to the parent, at most one turn per kInterval.
  class LevelWheel {
  public:
    static constexpr std::chrono::milliseconds kInterval{160};

    // `under` is the entry under the pointer, kNoNode for none, and `now` the time of the turn. Every vertical turn
    // is handled.
    PartReaction turn(
        const ShapeWheel& wheel, const workbench::Tree& tree, workbench::NodeId under,
        std::chrono::steady_clock::time_point now
    );

  private:
    std::optional<std::chrono::steady_clock::time_point> m_last;
  };

  // Moves every shape and label by (dx, dy).
  void offsetScene(workbench::SceneShapes& scene, float dx, float dy);

  // Fits the labels to their boxes with these metrics, as fitShapeLabels describes.
  void fitLabels(std::vector<workbench::ShapeLabel>& labels, const LabelMetrics& metrics);

  // The sentence a view shows over a current folder with no visible entries; empty when it has some.
  std::string folderNote(const workbench::Tree& tree, const workbench::LayoutSettings& settings);

  // A one-line label centered in `box`.
  workbench::ShapeLabel
  noteLabel(std::string text, const workbench::ShapeRect& box, const workbench::ColorPalette& palette);
} // namespace luciole
