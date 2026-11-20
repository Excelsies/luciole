#include "ui/workbench/ladder_part.h"

#include "ui/workbench/entry_tooltip.h"
#include "workbench/ladder_plan.h"

#include <optional>

namespace luciole {
  namespace {
    constexpr float kTransitionMs = 220;

    // Bars already sit against the bottom edge of the host they are planned for.
    class LadderPart final : public ScenePart {
    public:
      bool layout(const PartFrame& frame) override {
        m_scene = workbench::ladderScene(*frame.tree, frame.settings, frame.width, frame.height);
        m_frame = frame;
        return true;
      }

      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override {
        if (m_frame.tree == nullptr) {
          return {};
        }
        return workbench::ladderShapes(*m_frame.tree, m_frame.settings, m_scene, marks, m_frame.palette);
      }

      PartSnapshot shownSnapshot() const override { return {.tree = m_frame.tree, .serial = m_frame.serial}; }

      // The bars are the ancestors' entries; the current folder's own entries are not drawn.
      std::optional<workbench::ShapeRect> noteBox(float /*width*/, float /*height*/) const override {
        return std::nullopt;
      }

      TooltipContent tooltip(workbench::ShapeId id) const override {
        return m_tips.entry(m_frame.tree, &m_frame.settings, id);
      }

      float transitionMs() const override { return kTransitionMs; }

    private:
      PartFrame m_frame;
      workbench::LadderScene m_scene;
      mutable EntryTooltips m_tips;
    };
  } // namespace

  std::unique_ptr<ScenePart> makeLadderPart() { return std::make_unique<LadderPart>(); }
} // namespace luciole
