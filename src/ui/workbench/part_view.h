#pragma once

#include "render/scene/node.h"
#include "ui/workbench/scene_part.h"
#include "workbench/tree.h"

#include <cstdint>
#include <functional>
#include <memory>

class ShapeView;

namespace luciole {
  // A self-drawn Workbench pane: one shape view showing the scene of its part. The owner calls update() every frame;
  // the scene is laid out again only there, and only when the inputs, the kind, the size or the part's own view
  // changed. Hover, clicks, the wheel and keys go to the part and come back as requests.
  class PartView : public Node {
  public:
    PartView(const PartView&) = delete;
    PartView& operator=(const PartView&) = delete;
    PartView(PartView&&) = delete;
    PartView& operator=(PartView&&) = delete;
    ~PartView() override;

    // Lays the scene out again when something changed, at most once per call. A new snapshot or kind tweens to the
    // new scene unless `resizing`.
    void update(const PartInputs& inputs, bool resizing);
    // The node another pane points at, and the list's cursor; kNoNode for none.
    void setPointed(workbench::NodeId node);
    void setCursor(workbench::NodeId node);

    ShapeView& shapeView() const noexcept { return *m_view; }
    // Layouts started so far, a loading scene included.
    std::uint64_t sceneBuilds() const noexcept { return m_builds; }
    const PartInputs& inputs() const noexcept { return m_inputs; }
    // The snapshot of the scene shown, which can lag inputs() while a layout runs on a worker. Requests and hover
    // name its nodes; empty while loading.
    const PartSnapshot& shownSnapshot() const noexcept { return m_shown; }

    // The entry under the pointer changed; kNoNode when it left every entry.
    std::function<void(workbench::NodeId)> onHover;
    std::function<void(const PartRequest&)> onRequest;
    // Something waits for the next update(): the owner schedules a frame.
    std::function<void()> onFrameNeeded;

  protected:
    // `modifiers` reports the keyboard modifiers held.
    explicit PartView(std::function<std::uint32_t()> modifiers);

    // Replaces the part; the next update lays it out and tweens from what is shown.
    void setPart(std::unique_ptr<ScenePart> part);
    // A worker layout of the part finished.
    void partReady();
    void doLayout(Renderer& renderer) override;

  private:
    void build(float width, float height, bool resizing);
    void show();
    void request(const PartRequest& request);
    void react(const PartReaction& reaction);
    void needFrame();
    // The part asks to show its shapes again.
    void redraw(float transitionMs);

    ShapeView* m_view = nullptr;
    std::unique_ptr<ScenePart> m_part;
    LabelMetrics m_metrics;      // advances at the real text size
    LabelMetrics m_sceneMetrics; // the same, for the scene laid out in the view shrunk by the text scale
    float m_textScale = 1;
    float m_sceneScale = 1;   // the scale the part's scene was laid out at
    std::uint64_t m_metricsGeneration = 0;
    std::uint64_t m_metricsEpoch = 0;
    PartInputs m_inputs;
    PartSnapshot m_shown;
    float m_builtWidth = -1;
    float m_builtHeight = -1;
    float m_transitionMs = 0; // the tween the next fresh scene shows with
    float m_redrawMs = 0;     // the tween the part's next redraw shows with
    float m_pressX = 0;
    float m_pressY = 0;
    std::uint64_t m_builds = 0;
    bool m_dirty = true;      // lay out at the next update
    bool m_animate = false;   // the next layout comes from a new snapshot or kind
    bool m_reshape = false;   // make shapes at the next update
    bool m_fresh = false;     // the shapes to make come from a new layout
    bool m_loading = false;   // no snapshot yet
    bool m_shownOnce = false; // a scene was shown, so a new one can tween from it
  };
} // namespace luciole
