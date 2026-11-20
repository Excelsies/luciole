#include "ui/workbench/atlas_part.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "render/animation/motion_service.h"
#include "ui/workbench/atlas_animation.h"
#include "ui/workbench/atlas_keys.h"
#include "ui/workbench/atlas_layout_job.h"
#include "ui/workbench/atlas_picture.h"
#include "ui/workbench/entry_tooltip.h"
#include "workbench/atlas_camera.h"
#include "workbench/atlas_hud.h"
#include "workbench/atlas_motion.h"
#include "workbench/atlas_navigation.h"
#include "workbench/atlas_scene.h"
#include "workbench/circle_pack.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace luciole {
  namespace {
    using workbench::NodeId;
    using workbench::SceneRole;
    using Clock = std::chrono::steady_clock;

    constexpr double kKeyZoom = 1.6;
    constexpr double kNearFlightMs = 380;
    constexpr double kRecenterMs = 300;
    constexpr double kFarFadeMs = 260;
    constexpr std::array<const char*, 3> kButtonTips{"Zoom in (+)", "Zoom out (-)", "Recenter (0)"};

    bool sameMarks(const workbench::ShapeMarks& left, const workbench::ShapeMarks& right) {
      return left.hovered == right.hovered && left.linked == right.linked && left.cursor == right.cursor;
    }

    PartReaction handled() { return {.handled = true, .relayout = false, .requests = {}}; }

    // The camera lives in the current folder's frame of the shown snapshot, and so do the rules and every request.
    // Layouts run on a worker with their own packings, at most one at a time; a new snapshot is taken up when its
    // layout returns, and the rules then read a copy of the packings that layout made. Between layouts each frame
    // shows the last scene moved to the current camera, so panning, zooming and flights build nothing on the main
    // context; a layout follows once the scene drifts past its margin, and an exact one when the motion stops.
    class AtlasPart final : public ScenePart {
    public:
      explicit AtlasPart(std::function<void()> ready)
          : m_ready(std::move(ready)), m_packings(std::make_unique<workbench::PackingCache>()) {}
      AtlasPart(const AtlasPart&) = delete;
      AtlasPart& operator=(const AtlasPart&) = delete;
      AtlasPart(AtlasPart&&) = delete;
      AtlasPart& operator=(AtlasPart&&) = delete;
      ~AtlasPart() override { m_token.cancel(); }

      bool layout(const PartFrame& frame) override {
        const auto before = mapView();
        m_frame = frame;
        m_view = {.width = frame.width, .height = frame.height};
        m_font = frame.metrics.font(LabelMetrics::kBaseSize);
        m_nameFont = frame.metrics.font(LabelMetrics::kBaseSize, workbench::LabelWeight::Semibold);
        m_map = workbench::hudLayout(m_view, 0).map;
        m_cache.reset();
        if (m_tree != nullptr) {
          m_hud = atlasHud(*m_tree, m_view, m_font);
          const auto after = mapView();
          if (after.width != before.width || after.height != before.height) {
            followResize(before);
          }
        }
        start();
        return false;
      }

      void connect(const std::function<void()>& /*ready*/, const std::function<void(float)>& redraw) override {
        m_redraw = redraw;
      }

      // A cursor that moved asks for a frame, whose advance flies to rest when the view does not show its bubble well.
      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override {
        if (m_tree == nullptr) {
          return {};
        }
        if (marks.cursor != m_lastMarks.cursor && marks.cursor != workbench::kNoNode) {
          m_reveal = true;
          if (m_redraw) {
            m_redraw(0);
          }
        }
        m_lastMarks = marks;
        return composeAtlas(
            {
                .map = mapLayer(marks),
                .fading = m_motion.fading(),
                .fadeOpacity = m_motion.fadeOpacity(),
                .hud = workbench::atlasHudShapes(*m_tree, m_hud.layout, m_hud.placed, marks, m_frame.palette),
            },
            m_map, m_frame.palette
        );
      }

      PartSnapshot shownSnapshot() const override { return {.tree = m_tree, .serial = m_serial}; }

      // The wheel zooms about the pointer, eased over the next frames.
      PartReaction wheel(const ShapeWheel& wheel) override {
        if (m_tree == nullptr
            || !wheel.vertical
            || !std::isfinite(wheel.delta)
            || atlasControlAt(m_hud, m_map, wheel.x, wheel.y)) {
          return {};
        }
        m_motion.zoom(workbench::wheelLogZoom(wheel.delta), wheel.x - m_map.x, wheel.y - m_map.y);
        return handled();
      }

      PartReaction key(const InputArea::KeyData& key, const workbench::ShapeMarks& marks) override {
        if (m_tree == nullptr || (key.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0) {
          return {};
        }
        if (key.utf32 == '0') {
          pressButton(2);
          return handled();
        }
        if (key.utf32 == '+' || key.utf32 == '=' || KeySymbol::isPageUp(key.sym)) {
          pressButton(0);
          return handled();
        }
        if (key.utf32 == '-' || KeySymbol::isPageDown(key.sym)) {
          pressButton(1);
          return handled();
        }
        if (KeySymbol::isEnter(key.sym) || KeySymbol::isBackspace(key.sym)) {
          const auto request = KeySymbol::isEnter(key.sym) ? enterRequest(*m_tree, marks.cursor) : upRequest(*m_tree);
          auto reaction = handled();
          if (request) {
            reaction.requests.push_back(*request);
          }
          return reaction;
        }
        return atlasCursorKey(m_scene, key.sym, marks.cursor);
      }

      // A drag on the map pans: the point under the pointer follows it, and the rules may surface to the parent.
      PartReaction drag(const ShapeDrag& drag) override {
        if (m_tree == nullptr
            || (drag.phase == ShapeDragPhase::Begin && atlasControlAt(m_hud, m_map, drag.x, drag.y))) {
          return {};
        }
        auto reaction = handled();
        switch (drag.phase) {
        case ShapeDragPhase::Begin:
          m_motion.stop();
          m_dragging = true;
          m_dragX = drag.x;
          m_dragY = drag.y;
          break;
        case ShapeDragPhase::Move: {
          auto camera = m_camera;
          camera.cx -= (drag.x - m_dragX) / camera.scale;
          camera.cy -= (drag.y - m_dragY) / camera.scale;
          m_dragX = drag.x;
          m_dragY = drag.y;
          apply(workbench::afterMove(source(), m_cwd, camera, mapView(), false, 0, 0), reaction);
          break;
        }
        case ShapeDragPhase::End:
          m_dragging = false;
          break;
        }
        refresh();
        return reaction;
      }

      // Chips go to their folder, the buttons zoom or recenter, empty space goes up a level, and the bar does nothing.
      PartReaction activate(workbench::ShapeId id) override {
        if (m_tree == nullptr) {
          return {};
        }
        auto reaction = handled();
        const auto role = workbench::sceneRole(id.role);
        if (id == atlasBarId()) {
          return reaction;
        }
        if (id == atlasBackgroundId()) {
          if (const auto up = upRequest(*m_tree)) {
            reaction.requests.push_back(*up);
          }
        } else if (role == SceneRole::Chip && workbench::hasNode(*m_tree, id.node) && id.node != m_tree->root) {
          reaction.requests.push_back({.kind = PartRequestKind::Go, .node = id.node});
        } else if (role == SceneRole::Button) {
          pressButton(id.role & 0xFFU);
        } else {
          return {};
        }
        return reaction;
      }

      TooltipContent tooltip(workbench::ShapeId id) const override {
        const auto role = workbench::sceneRole(id.role);
        if (role == SceneRole::Chip && m_tree != nullptr && workbench::hasNode(*m_tree, id.node)) {
          return workbench::hudPath(*m_tree, id.node);
        }
        if (role == SceneRole::Button && (id.role & 0xFFU) < kButtonTips.size()) {
          return std::string(kButtonTips[id.role & 0xFFU]);
        }
        // The circles move under a still pointer while the camera does, so an entry says nothing until it rests.
        if (m_tree == nullptr || moving() || m_dragging) {
          return {};
        }
        return m_tips.entry(m_tree, &m_settings, id);
      }

      bool moving() const override { return m_motion.moving() || m_reveal; }

      PartReaction advance(Clock::time_point now) override {
        auto reaction = handled();
        if (std::exchange(m_reveal, false)
            && m_tree != nullptr
            && !m_motion.flying()
            && !m_dragging
            && workbench::cursorNeedsReveal(source(), m_cwd, m_lastMarks.cursor, m_camera, mapView())) {
          m_motion.fly(m_camera, workbench::restCamera(mapView()), kRecenterMs);
        }
        const auto step = m_motion.step(now, !MotionService::instance().enabled());
        if (m_tree != nullptr && step.logZoom != 0) {
          const auto state =
              workbench::zoomAt(source(), m_cwd, m_camera, mapView(), std::exp(step.logZoom), step.x, step.y);
          apply(state, reaction);
        }
        if (step.camera) {
          m_camera = *step.camera;
        }
        refresh();
        return reaction;
      }

      float transitionMs() const override { return 0; }

    private:
      workbench::AtlasSource source() { return {.tree = *m_tree, .settings = m_settings, .packings = m_nav}; }
      workbench::ViewSize mapView() const { return {.width = m_map.width, .height = m_map.height}; }

      // The scene's shapes in the map's coordinates, built again only for another scene or other marks.
      const workbench::SceneShapes& sceneShapes(const workbench::ShapeMarks& marks) const {
        if (!m_cache || m_cache->serial != m_sceneSerial || !sameMarks(m_cache->marks, marks)) {
          m_cache = ShapeCache{
              .serial = m_sceneSerial,
              .marks = marks,
              .shapes = workbench::atlasShapes(*m_tree, m_settings, m_scene, marks, m_frame.palette, m_nameFont),
          };
        }
        return m_cache->shapes;
      }

      workbench::SceneShapes mapLayer(const workbench::ShapeMarks& marks) const {
        const auto frame =
            m_builtCamera ? workbench::reframe(*m_builtCamera, m_camera, mapView()) : workbench::Reframe{};
        return atlasMapLayer(sceneShapes(marks), frame, m_map);
      }

      // Zoom in, zoom out about the map's center, or fly back to the rest view.
      void pressButton(std::size_t index) {
        if (index == 0 || index == 1) {
          m_motion.zoom((index == 0 ? 1 : -1) * std::log(kKeyZoom), m_map.width / 2, m_map.height / 2);
        } else if (index == 2) {
          m_motion.fly(m_camera, workbench::restCamera(mapView()), kRecenterMs);
        }
      }

      // After the map changed size the folder keeps its share of the view; a flight under way ends at rest.
      void followResize(workbench::ViewSize before) {
        m_motion.dropFade();
        if (m_motion.flying()) {
          m_motion.stop();
          m_camera = workbench::restCamera(mapView());
        } else {
          m_camera.scale *= workbench::restScale(mapView()) / workbench::restScale(before);
        }
        m_camera = workbench::clampCamera(source(), m_cwd, m_camera, mapView(), 0, 0);
      }

      // Takes up where the rules left the camera, keeping the shown scene's camera in the same frame.
      void apply(const workbench::CameraState& state, PartReaction& reaction) {
        if (state.cwd != m_cwd) {
          ++m_cwdMoves;
          const auto sim = m_builtCamera ? workbench::similarity(source(), m_cwd, state.cwd) : std::nullopt;
          m_builtCamera = sim ? std::optional(workbench::convert(*m_builtCamera, *sim)) : std::nullopt;
        }
        m_cwd = state.cwd;
        m_camera = state.camera;
        for (const auto step : state.steps) {
          reaction.requests.push_back({.kind = PartRequestKind::Go, .node = step});
        }
      }

      // Asks for a layout when the shown scene no longer serves: while moving, when it has no margin or drifted past
      // it and no layout is on its way; at rest, unless the latest layout asked for is of the current camera.
      void refresh() {
        if (m_tree == nullptr || m_frame.tree == nullptr) {
          return;
        }
        if (moving() || m_dragging) {
          const bool stale = !m_builtCamera
              || m_sceneMargin == 0
              || workbench::reframeStale(workbench::reframe(*m_builtCamera, m_camera, mapView()), mapView());
          if (stale && m_packings != nullptr) {
            start();
          }
          return;
        }
        if (m_askedCwd != m_cwd || m_askedCamera != m_camera) {
          start();
        }
      }

      // Lays the scene out on a worker; one at a time, the latest inputs following when it returns.
      void start() {
        if (m_packings == nullptr) {
          m_again = true;
          return;
        }
        m_again = false;
        AtlasJob job{
            .tree = m_frame.tree,
            .serial = m_frame.serial,
            .settings = m_frame.settings,
            .view = mapView(),
            .adopt = m_frame.tree != m_tree,
            .names = {},
            .cwd = m_cwd,
            .camera = m_camera,
            .cwdMoves = m_cwdMoves,
            .margin = moving() || m_dragging ? workbench::kAtlasMarginShare : 0.0,
        };
        if (job.adopt && m_tree != nullptr) {
          job.names = atlasNamesTo(*m_tree, m_cwd);
        }
        m_askedCwd = m_cwd;
        m_askedCamera = m_camera;
        if (m_packingsTree != m_frame.tree) {
          m_packings->clear();
          m_packingsTree = m_frame.tree;
        }
        runJob<AtlasBuilt>(
            m_token,
            [job = std::move(job), packings = std::move(m_packings)] mutable {
              return buildAtlasLayout(std::move(job), std::move(packings));
            },
            [this, alive = std::weak_ptr<int>(m_lifetime)](JobResult<AtlasBuilt> result) {
              if (!alive.expired()) {
                finish(std::move(result));
              }
            }
        );
      }

      void finish(JobResult<AtlasBuilt> result) {
        const bool shown = result && take(*result);
        if (result) {
          m_packings = std::move(result->packings);
        } else {
          m_packings = std::make_unique<workbench::PackingCache>();
          m_packingsTree = nullptr;
        }
        if (shown) {
          m_nav = *m_packings;
        }
        if (m_again) {
          start();
        } else {
          refresh();
        }
        if (shown && m_ready) {
          m_ready();
        }
      }

      // Shows a layout of the latest snapshot. A new snapshot whose camera started from a folder the rules have
      // since left is laid out again instead. Arriving from near by flies the camera to rest; from far, the old
      // picture fades out over the new one at rest.
      bool take(AtlasBuilt& built) {
        auto& job = built.job;
        if (job.tree != m_frame.tree) {
          return false;
        }
        if (job.adopt) {
          if (job.cwdMoves != m_cwdMoves) {
            m_again = true;
            return false;
          }
          adopt(built);
        } else if (job.tree != m_tree) {
          return false;
        }
        m_serial = job.serial;
        m_settings = job.settings;
        m_scene = std::move(built.scene);
        m_sceneMargin = job.margin;
        ++m_sceneSerial;
        m_builtCamera = job.camera;
        if (job.cwd != m_cwd) {
          const auto sim = workbench::similarity(source(), job.cwd, m_cwd);
          m_builtCamera = sim ? std::optional(workbench::convert(job.camera, *sim)) : std::nullopt;
        }
        m_cache.reset();
        return true;
      }

      void adopt(const AtlasBuilt& built) {
        const auto& job = built.job;
        const bool first = m_tree == nullptr;
        if (!first && built.arrival == AtlasArrival::Far) {
          m_motion.fade(
              atlasPicture(mapLayer(m_lastMarks), m_motion.fading(), m_motion.fadeOpacity(), m_map, m_frame.palette),
              kFarFadeMs
          );
        }
        // The camera may have moved in the old frame since the job started; the same picture in the new one. The map
        // may have changed size too: a camera at rest rests in the map as it is now.
        const auto rest = workbench::restCamera(mapView());
        const auto camera = built.change ? workbench::convert(m_camera, *built.change) : rest;
        m_camera = workbench::usableCamera(camera) ? camera : rest;
        m_cwd = job.cwd;
        m_tree = job.tree;
        m_askedCwd = job.cwd;
        m_askedCamera = job.camera;
        m_hud = atlasHud(*m_tree, m_view, m_font);
        // The same folder again keeps its frame, so a flight, zoom or fade under way goes on.
        if (built.arrival != AtlasArrival::Same) {
          m_motion.stop();
        }
        if (!first && built.arrival == AtlasArrival::Near) {
          m_motion.fly(m_camera, rest, kNearFlightMs);
        }
      }

      struct ShapeCache {
        std::uint64_t serial = 0;
        workbench::ShapeMarks marks;
        workbench::SceneShapes shapes;
      };

      std::function<void()> m_ready;
      std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
      CancelToken m_token;
      PartFrame m_frame; // the latest inputs
      workbench::ViewSize m_view;
      workbench::Rect m_map; // where the circles go: the view, or below the bar in a small view
      workbench::LabelFont m_font;
      workbench::LabelFont m_nameFont; // semibold, as the circles' names are drawn
      AtlasHud m_hud;
      // The shown snapshot, its settings and scene, and the camera in its current folder's frame.
      std::shared_ptr<const workbench::Tree> m_tree;
      std::uint64_t m_serial = 0;
      workbench::LayoutSettings m_settings;
      workbench::AtlasScene m_scene;
      std::uint64_t m_sceneSerial = 0;                // counts scenes taken
      double m_sceneMargin = 0;                       // how far beyond the view the scene reaches
      std::optional<workbench::Camera> m_builtCamera; // the scene's camera in m_cwd's frame
      NodeId m_cwd = workbench::kNoNode;
      workbench::Camera m_camera;
      std::uint64_t m_cwdMoves = 0;           // counts changes of m_cwd
      NodeId m_askedCwd = workbench::kNoNode; // the latest layout asked for
      workbench::Camera m_askedCamera;
      workbench::PackingCache m_nav;                         // for the rules, on the main context
      std::unique_ptr<workbench::PackingCache> m_packings;   // for layouts; with the worker while one runs
      std::shared_ptr<const workbench::Tree> m_packingsTree; // the snapshot m_packings was filled from
      bool m_again = false;
      AtlasAnimation m_motion;
      bool m_dragging = false; // a drag pans the camera
      double m_dragX = 0;
      double m_dragY = 0;
      mutable std::optional<ShapeCache> m_cache;
      mutable workbench::ShapeMarks m_lastMarks; // the marks the shapes were last asked with
      mutable EntryTooltips m_tips;
      mutable bool m_reveal = false; // the cursor moved since the last advance
      std::function<void(float)> m_redraw;
    };
  } // namespace

  std::unique_ptr<ScenePart> makeAtlasPart(std::function<void()> ready) {
    return std::make_unique<AtlasPart>(std::move(ready));
  }
} // namespace luciole
