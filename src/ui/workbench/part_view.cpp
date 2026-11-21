#include "ui/workbench/part_view.h"

#include "ui/controls/shape_view.h"
#include "ui/workbench/text_scale.h"
#include "workbench/scene_zoom.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace luciole {
  namespace {
    // True when every node id of `scene` that both snapshots hold names the same entry in each, up to the top of
    // the snapshot, so shapes can pair by id. Each node is compared once.
    bool sameIds(const workbench::Tree& before, const workbench::Tree& after, const workbench::SceneShapes& scene) {
      const std::size_t shared = std::min(before.nodes.size(), after.nodes.size());
      std::unordered_set<workbench::NodeId> checked;
      const auto same = [&](workbench::NodeId id) {
        // Each step moves to a parent not yet checked: at most `shared` steps.
        for (std::size_t steps = 0; id < shared && steps < shared && checked.insert(id).second; ++steps) {
          const auto& left = before.nodes[id];
          const auto& right = after.nodes[id];
          if (left.parent != right.parent || left.name != right.name) {
            return false;
          }
          id = right.parent;
        }
        return true;
      };
      return std::ranges::all_of(scene.shapes, [&](const workbench::Shape& shape) { return same(shape.id.node); })
          && std::ranges::all_of(scene.labels, [&](const workbench::ShapeLabel& label) { return same(label.node); });
    }
  } // namespace

  PartView::PartView(std::function<std::uint32_t()> modifiers)
      : m_view(static_cast<ShapeView*>(addChild(std::make_unique<ShapeView>(std::move(modifiers))))) {
    m_view->onHoverChanged = [this](std::optional<workbench::ShapeId> id) {
      if (m_part != nullptr) {
        m_part->hover(id);
      }
      if (onHover && !id) {
        onHover(workbench::kNoNode);
      } else if (onHover) {
        onHover(m_part != nullptr ? m_part->pointedNode(*id) : hoveredNode(*id));
      }
    };
    m_view->onMarksChanged = [this] {
      m_reshape = true;
      needFrame();
    };
    m_view->onPress = [this](workbench::ShapeId /*id*/, const InputArea::PointerData& pointer) {
      m_pressX = pointer.localX;
      m_pressY = pointer.localY;
    };
    m_view->onActivate = [this](workbench::ShapeId id, std::uint32_t button, std::uint32_t modifiers) {
      if (m_shown.tree == nullptr) {
        return;
      }
      if (const auto own = m_part != nullptr ? m_part->activate(id) : PartReaction{}; own.handled) {
        react(own);
        return;
      }
      if (auto ask = clickRequest(*m_shown.tree, id)) {
        ask->x = m_pressX;
        ask->y = m_pressY;
        ask->button = button;
        ask->modifiers = modifiers;
        request(*ask);
      }
    };
    m_view->onContextRequest = [this](std::optional<workbench::ShapeId> id, float x, float y) {
      request({
          .kind = PartRequestKind::Context,
          .node = id ? id->node : workbench::kNoNode,
          .x = x,
          .y = y,
          .button = 0,
          .modifiers = 0,
      });
    };
    m_view->onWheel = [this](const ShapeWheel& wheel) {
      if (m_part == nullptr || m_loading) {
        return false;
      }
      ShapeWheel inScene = wheel;
      inScene.x /= m_sceneScale;
      inScene.y /= m_sceneScale;
      const auto reaction = m_part->wheel(inScene);
      react(reaction);
      return reaction.handled;
    };
    m_view->onKey = [this](const InputArea::KeyData& key) {
      if (m_part == nullptr || m_loading || !key.pressed) {
        return;
      }
      react(m_part->key(key, m_view->marks()));
    };
    m_view->onDrag = [this](const ShapeDrag& drag) {
      if (m_part == nullptr || m_loading) {
        return false;
      }
      ShapeDrag inScene = drag;
      inScene.x /= m_sceneScale;
      inScene.y /= m_sceneScale;
      const auto reaction = m_part->drag(inScene);
      react(reaction);
      if (reaction.handled) {
        m_reshape = true;
        needFrame();
      }
      return reaction.handled;
    };
    m_view->setTooltipProvider([this](workbench::ShapeId id) {
      return m_part == nullptr || m_loading ? TooltipContent{} : m_part->tooltip(id);
    });
    m_view->onResize = [this](float, float) { needFrame(); };
  }

  PartView::~PartView() {
    // The part may hold a worker's callback; it goes before the view it reports to.
    m_part.reset();
  }

  void PartView::setPart(std::unique_ptr<ScenePart> part) {
    m_part = std::move(part);
    if (m_part != nullptr) {
      m_part->connect([this] { partReady(); }, [this](float transitionMs) { redraw(transitionMs); });
    }
    m_dirty = true;
    m_animate = true;
    needFrame();
  }

  void PartView::partReady() {
    if (m_awaiting) {
      m_sceneScale = m_pendingScale;
      m_awaiting = false;
    }
    m_reshape = true;
    m_fresh = true;
    needFrame();
  }

  void PartView::update(const PartInputs& inputs, bool resizing) {
    const auto& before = m_inputs;
    const bool newTree =
        inputs.snapshot.tree != before.snapshot.tree || inputs.snapshot.serial != before.snapshot.serial;
    const bool restyled = inputs.settings != before.settings || !samePalette(inputs.palette, before.palette);
    if (newTree || restyled || inputs.trail != before.trail) {
      m_dirty = true;
      m_animate = m_animate || newTree;
      m_inputs = inputs;
    }
    const float width = m_view->width();
    const float height = m_view->height();
    m_dirty = m_dirty || width != m_builtWidth || height != m_builtHeight;
    if (m_dirty && m_part != nullptr && m_metrics.ready() && width > 0 && height > 0) {
      build(width, height, resizing);
    }
    if (m_part != nullptr && !m_loading && m_part->moving()) {
      react(m_part->advance(std::chrono::steady_clock::now()));
      m_reshape = true;
    }
    if (m_reshape && (m_fresh || !m_view->transitioning())) {
      show();
    }
  }

  void PartView::build(float width, float height, bool resizing) {
    m_dirty = false;
    m_builtWidth = width;
    m_builtHeight = height;
    ++m_builds;
    // A layout for the wheel or a restyle keeps the tween a pending worker layout still owes.
    const float transition = m_animate && m_shownOnce ? m_part->transitionMs() : 0;
    m_transitionMs = resizing ? 0 : std::max(m_transitionMs, transition);
    m_animate = false;
    m_loading = m_inputs.snapshot.tree == nullptr;
    if (m_loading) {
      m_reshape = true;
      m_fresh = true;
      return;
    }
    const PartFrame frame{
        .tree = m_inputs.snapshot.tree,
        .serial = m_inputs.snapshot.serial,
        .settings = m_inputs.settings,
        .palette = m_inputs.palette,
        .width = width / m_textScale,
        .height = height / m_textScale,
        .metrics = m_sceneMetrics,
        .metricsEpoch = m_metricsEpoch,
        .trail = m_inputs.trail,
    };
    if (m_part->layout(frame)) {
      m_sceneScale = m_textScale;
      m_awaiting = false;
      m_reshape = true;
      m_fresh = true;
    } else if (m_part->holdsSceneUntilReply()) {
      m_pendingScale = m_textScale; // the scene shown stays at its own scale until the worker's reply
      m_awaiting = true;
    } else {
      m_sceneScale = m_textScale;
    }
  }

  void PartView::show() {
    workbench::SceneShapes scene;
    PartSnapshot shown;
    std::string note = "Loading…";
    // The scene lives in the view as the text scale shrinks it; zoomScene brings it to the real one.
    const float sceneWidth = m_builtWidth / m_sceneScale;
    const float sceneHeight = m_builtHeight / m_sceneScale;
    std::optional<workbench::ShapeRect> box =
        workbench::ShapeRect{.x = 0, .y = 0, .width = sceneWidth, .height = sceneHeight, .radius = 0};
    if (!m_loading) {
      scene = m_part->shapes(m_view->marks());
      shown = m_part->shownSnapshot();
      if (shown.tree != nullptr) {
        note = folderNote(*shown.tree, m_inputs.settings);
        box = m_part->noteBox(sceneWidth, sceneHeight);
      }
    }
    // A note the scene writes itself is not written twice.
    const bool written = std::ranges::any_of(scene.labels, [&note](const auto& label) { return label.text == note; });
    if (!note.empty() && box && !written) {
      scene.labels.insert(scene.labels.begin(), noteLabel(std::move(note), *box, m_inputs.palette));
    }
    workbench::zoomScene(scene, m_sceneScale);
    fitLabels(scene.labels, m_metrics);
    const float redrawn = std::exchange(m_redrawMs, 0.0F);
    float transition = m_fresh ? std::exchange(m_transitionMs, 0.0F) : redrawn;
    const bool otherIds = shown.tree != nullptr
        && m_shown.tree != nullptr
        && shown.tree != m_shown.tree
        && !sameIds(*m_shown.tree, *shown.tree, scene);
    if (otherIds) {
      transition = 0;
    }
    m_shown = std::move(shown);
    m_reshape = false;
    m_fresh = false;
    m_shownOnce = true;
    // Showing may move the hover, which asks for another reshape.
    m_view->setScene(std::move(scene), transition);
  }

  void PartView::setPointed(workbench::NodeId node) { m_view->setHighlighted(node); }

  void PartView::setCursor(workbench::NodeId node) { m_view->setCursor(node); }

  void PartView::request(const PartRequest& request) {
    if (onRequest) {
      onRequest(request);
    }
  }

  void PartView::react(const PartReaction& reaction) {
    m_dirty = m_dirty || reaction.relayout;
    if (reaction.relayout || (m_part != nullptr && m_part->moving())) {
      needFrame();
    }
    for (auto each : reaction.requests) {
      each.x *= m_sceneScale; // the part works in the view the scene was laid out in
      each.y *= m_sceneScale;
      request(each);
    }
  }

  void PartView::redraw(float transitionMs) {
    m_reshape = true;
    m_redrawMs = std::max(m_redrawMs, transitionMs);
    needFrame();
  }

  void PartView::needFrame() {
    if (onFrameNeeded) {
      onFrameNeeded();
    }
  }

  void PartView::doLayout(Renderer& renderer) {
    // The label widths and the scene's scale follow the system font: a change lays the scene out again.
    const float scale = systemTextScale();
    const auto generation = renderer.textMetricsGeneration();
    if (!m_metrics.ready() || scale != m_textScale || generation != m_metricsGeneration) {
      m_metrics.measure(renderer);
      m_textScale = scale;
      m_metricsGeneration = generation;
      ++m_metricsEpoch;
      m_sceneMetrics = m_metrics.scaled(1.0 / static_cast<double>(scale));
      m_dirty = true;
      needFrame();
    }
    m_view->setFrameSize(width(), height());
    Node::doLayout(renderer);
  }
} // namespace luciole
