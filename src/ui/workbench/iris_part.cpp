#include "ui/workbench/iris_part.h"

#include "core/input/key_symbols.h"
#include "workbench/iris_facts.h"
#include "workbench/iris_hit.h"
#include "workbench/iris_keys.h"
#include "workbench/iris_scene.h"
#include "workbench/ring_geometry.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole {
  namespace {
    using workbench::NodeId;
    using workbench::iris::CursorStop;
    using workbench::iris::KeyMove;
    using workbench::iris::SectorKind;

    constexpr float kTransitionMs = 260;
    constexpr double kArcFont = 11;
    constexpr std::string_view kAdvanceSample = "abcdefghijklmnopqrstuvwxyz";

    std::optional<KeyMove> keyMove(std::uint32_t sym) {
      if (KeySymbol::isDown(sym)) {
        return KeyMove::Down;
      }
      if (KeySymbol::isUp(sym)) {
        return KeyMove::Up;
      }
      if (KeySymbol::isPageDown(sym)) {
        return KeyMove::PageDown;
      }
      if (KeySymbol::isPageUp(sym)) {
        return KeyMove::PageUp;
      }
      if (KeySymbol::isHome(sym)) {
        return KeyMove::Home;
      }
      if (KeySymbol::isEnd(sym)) {
        return KeyMove::End;
      }
      if (KeySymbol::isLeft(sym)) {
        return KeyMove::Left;
      }
      if (KeySymbol::isRight(sym)) {
        return KeyMove::Right;
      }
      return std::nullopt;
    }

    TooltipContent tooltipOf(workbench::iris::Tip tip) {
      if (!tip.rows.empty()) {
        std::vector<TooltipRow> rows;
        rows.reserve(tip.rows.size());
        for (auto& row : tip.rows) {
          rows.push_back(
              {.key = std::move(row.key), .value = std::move(row.value), .valueEllipsize = TextEllipsize::End}
          );
        }
        return rows;
      }
      if (!tip.text.empty()) {
        return std::move(tip.text);
      }
      return {};
    }

    class IrisPart final : public ScenePart {
    public:
      bool layout(const PartFrame& frame) override {
        const bool compact = workbench::ring::chooseCompact(frame.width, frame.height, m_compact);
        // Only the part's own view changed, the keyboard's stop on "+N more": the scene stays.
        if (m_scene && sameInputs(frame) && m_compact == compact) {
          return true;
        }
        const auto& tree = *frame.tree;
        workbench::iris::SceneInput input{
            .width = frame.width,
            .height = frame.height,
            .compact = compact,
            .settings = frame.settings,
            .filter = {},
            .palette = frame.palette,
            .trail = frame.trail.folders,
            .font = std::nullopt,
            .boldFont = std::nullopt,
        };
        if (frame.metrics.ready()) {
          input.font = frame.metrics.font(workbench::iris::TextMeasure::kBaseSize);
          input.boldFont =
              frame.metrics.font(workbench::iris::TextMeasure::kBaseSize, workbench::LabelWeight::Semibold);
          const double advance =
              frame.metrics.width(kAdvanceSample, kArcFont) / static_cast<double>(kAdvanceSample.size());
          if (std::isfinite(advance) && advance > 0) {
            input.labelAdvance = advance;
          }
        }
        m_scene = workbench::iris::buildScene(tree, input);
        m_compact = compact;
        m_frame = frame;
        // A new folder forgets the keyboard's stop on "+N more".
        auto folder = workbench::iris::nodePath(tree, tree.root);
        if (folder != m_folder) {
          m_onMore = false;
          m_folder = std::move(folder);
        }
        return true;
      }

      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override {
        if (m_frame.tree == nullptr || !m_scene) {
          return {};
        }
        return workbench::irisShapes(*m_frame.tree, *m_scene, marks, m_frame.palette, stopFor(marks));
      }

      PartSnapshot shownSnapshot() const override { return {.tree = m_frame.tree, .serial = m_frame.serial}; }

      // The hub writes the note itself; a hub too small for text writes none, and nothing is written over the dial.
      std::optional<workbench::ShapeRect> noteBox(float width, float height) const override {
        if (m_scene) {
          return std::nullopt;
        }
        return ScenePart::noteBox(width, height);
      }

      PartReaction wheel(const ShapeWheel& wheel) override {
        if (m_frame.tree == nullptr || !m_scene) {
          return {};
        }
        NodeId under = workbench::kNoNode;
        const auto hit = workbench::iris::hitTest(*m_scene, {.x = wheel.x, .y = wheel.y});
        const bool main = hit
            && (hit->kind == workbench::iris::HitKind::Arc || hit->kind == workbench::iris::HitKind::Row)
            && hit->sector < m_scene->main.sectors.size();
        if (main) {
          under = m_scene->main.sectors[hit->sector].node;
        }
        return m_wheel.turn(wheel, *m_frame.tree, under, std::chrono::steady_clock::now());
      }

      PartReaction key(const InputArea::KeyData& key, const workbench::ShapeMarks& marks) override {
        if (m_frame.tree == nullptr || !m_scene || key.modifiers != 0) {
          return {};
        }
        const auto& tree = *m_frame.tree;
        const auto stop = stopFor(marks);
        m_onMore = stop == CursorStop::More;
        PartReaction reaction{.handled = true, .relayout = false, .requests = {}};
        if (KeySymbol::isEnter(key.sym)) {
          const auto request = m_onMore ? std::optional(moreRequest()) : enterRequest(tree, marks.cursor);
          if (request) {
            reaction.requests.push_back(*request);
          }
          return reaction;
        }
        if (KeySymbol::isBackspace(key.sym)) {
          if (const auto request = upRequest(tree)) {
            reaction.requests.push_back(*request);
          }
          return reaction;
        }
        const auto move = keyMove(key.sym);
        if (!move) {
          return {};
        }
        const auto from = workbench::iris::cursorRow(*m_scene, marks.cursor, stop);
        const auto target = workbench::iris::keyTarget(*m_scene, from, *move);
        if (!target || *target >= m_scene->main.sectors.size()) {
          return reaction;
        }
        const auto& sector = m_scene->main.sectors[*target];
        const bool onMore = sector.kind == SectorKind::More;
        // The stop on "+N more" is the part's own: it lasts while the list's cursor stays where it was.
        reaction.relayout = onMore != m_onMore;
        m_onMore = onMore;
        m_moreBase = marks.cursor;
        if (!onMore) {
          reaction.requests.push_back({.kind = PartRequestKind::Select, .node = sector.node});
        }
        return reaction;
      }

      // A row of the ancestor stack goes up to its folder and a trail chip down to its folder.
      PartReaction activate(workbench::ShapeId id) override {
        if (m_frame.tree == nullptr || !m_scene) {
          return {};
        }
        const auto folder = workbench::iris::folderOf(*m_scene, id);
        if (!workbench::hasNode(*m_frame.tree, folder)) {
          return {};
        }
        return {
            .handled = true,
            .relayout = false,
            .requests = {{.kind = PartRequestKind::Go, .node = folder}},
        };
      }

      TooltipContent tooltip(workbench::ShapeId id) const override {
        if (m_frame.tree == nullptr || !m_scene) {
          return {};
        }
        return tooltipOf(workbench::iris::shapeTip(*m_frame.tree, *m_scene, id));
      }

      // The row of "+N more" carries the folder's node but stands for no single entry.
      NodeId pointedNode(workbench::ShapeId id) const override {
        const bool moreRow =
            m_scene && workbench::sceneRole(id.role) == workbench::SceneRole::Row && id.node == m_scene->folder;
        return moreRow ? workbench::kNoNode : ScenePart::pointedNode(id);
      }

      float transitionMs() const override { return kTransitionMs; }

    private:
      // The list of "+N more", opening at its piece in view coordinates.
      PartRequest moreRequest() const {
        const auto at = workbench::iris::moreAnchor(*m_scene);
        return {
            .kind = PartRequestKind::ShowMore,
            .node = m_scene->folder,
            .x = static_cast<float>(at.x),
            .y = static_cast<float>(at.y),
            .button = 0,
            .modifiers = 0,
            .anchored = true,
        };
      }

      CursorStop stopFor(const workbench::ShapeMarks& marks) const {
        return m_onMore && marks.cursor == m_moreBase ? CursorStop::More : CursorStop::Marks;
      }

      bool sameInputs(const PartFrame& frame) const {
        return frame.tree == m_frame.tree
            && frame.serial == m_frame.serial
            && frame.settings == m_frame.settings
            && samePalette(frame.palette, m_frame.palette)
            && frame.width == m_frame.width
            && frame.height == m_frame.height
            && frame.metrics.ready() == m_frame.metrics.ready()
            && frame.metricsEpoch == m_frame.metricsEpoch
            && frame.trail == m_frame.trail;
      }

      PartFrame m_frame;
      std::optional<workbench::iris::Scene> m_scene;
      std::optional<bool> m_compact;
      LevelWheel m_wheel;
      std::string m_folder;                   // path of the folder shown, to tell a new folder from a new snapshot
      bool m_onMore = false;                  // the keyboard's cursor rests on "+N more"
      NodeId m_moreBase = workbench::kNoNode; // the list's cursor when it came to rest there
    };
  } // namespace

  std::unique_ptr<ScenePart> makeIrisPart() { return std::make_unique<IrisPart>(); }
} // namespace luciole
