#include "ui/workbench/flow_part.h"

#include "ui/workbench/entry_tooltip.h"
#include "workbench/flow_layout.h"

#include <algorithm>
#include <ranges>

namespace luciole {
  namespace {
    constexpr float kTransitionMs = 260;

    class FlowPart final : public ScenePart {
    public:
      bool layout(const PartFrame& frame) override {
        const double height = std::min(frame.height, workbench::kFlowLimits.maxHeight);
        m_layout = workbench::flowLayout(*frame.tree, frame.settings, frame.width, height);
        m_offset = static_cast<float>(std::max(frame.height - height, 0.0));
        m_frame = frame;
        m_font = frame.metrics.font(LabelMetrics::kBaseSize);
        return true;
      }

      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override {
        if (m_frame.tree == nullptr) {
          return {};
        }
        auto scene = workbench::flowShapes(*m_frame.tree, m_frame.settings, m_layout, marks, m_frame.palette, m_font);
        offsetScene(scene, 0, m_offset);
        return scene;
      }

      PartSnapshot shownSnapshot() const override { return {.tree = m_frame.tree, .serial = m_frame.serial}; }

      // A More record is the folders its parent's bar leaves out; any other bar or ribbon is one entry.
      TooltipContent tooltip(workbench::ShapeId id) const override {
        if (m_frame.tree == nullptr) {
          return {};
        }
        const auto role = workbench::sceneRole(id.role);
        if (role != workbench::SceneRole::More && role != workbench::SceneRole::MoreRibbon) {
          return m_tips.entry(m_frame.tree, &m_frame.settings, id);
        }
        const auto& records = m_layout.topology.records;
        const auto more = std::ranges::find_if(records, [&id](const workbench::FlowRecord& record) {
          return record.kind == workbench::FlowRecordKind::More && record.node == id.node;
        });
        return more == records.end() ? TooltipContent{} : m_tips.merged(m_frame.tree, id, more->folders);
      }

      float transitionMs() const override { return kTransitionMs; }

    private:
      PartFrame m_frame;
      workbench::FlowLayout m_layout;
      workbench::LabelFont m_font; // label advances at LabelMetrics::kBaseSize
      float m_offset = 0;
      mutable EntryTooltips m_tips;
    };
  } // namespace

  std::unique_ptr<ScenePart> makeFlowPart() { return std::make_unique<FlowPart>(); }
} // namespace luciole
