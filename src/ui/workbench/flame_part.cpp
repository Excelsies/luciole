#include "ui/workbench/flame_part.h"

#include "ui/workbench/entry_tooltip.h"
#include "workbench/flame_bands.h"
#include "workbench/flame_scene.h"

#include <algorithm>
#include <optional>

namespace luciole {
  namespace {
    constexpr float kTransitionMs = 240;
    constexpr double kFirstAheadFont = 13;
    constexpr double kLabelFont = 12;
    constexpr double kValueFont = 11;

    class FlamePart final : public ScenePart {
    public:
      bool layout(const PartFrame& frame) override {
        const double band = workbench::flameBandHeight(frame.height);
        const workbench::FlameFonts fonts{
            .firstAhead = frame.metrics.font(kFirstAheadFont),
            .ahead = frame.metrics.font(kLabelFont),
            .ancestor = frame.metrics.font(kLabelFont),
            .value = frame.metrics.font(kValueFont),
        };
        m_scene = workbench::flameScene(*frame.tree, frame.settings, frame.width, band, fonts);
        m_offset = static_cast<float>(std::max(frame.height - band, 0.0));
        m_frame = frame;
        return true;
      }

      workbench::SceneShapes shapes(const workbench::ShapeMarks& marks) const override {
        if (m_frame.tree == nullptr) {
          return {};
        }
        auto scene = workbench::flameShapes(*m_frame.tree, m_frame.settings, m_scene, marks, m_frame.palette);
        offsetScene(scene, 0, m_offset);
        return scene;
      }

      PartSnapshot shownSnapshot() const override { return {.tree = m_frame.tree, .serial = m_frame.serial}; }

      // Over the strata ahead, where the folder's entries would be; none when the band has no room for them.
      std::optional<workbench::ShapeRect> noteBox(float /*width*/, float /*height*/) const override {
        const auto& box = m_scene.aheadBox;
        if (!box) {
          return std::nullopt;
        }
        const auto f = [](double value) { return static_cast<float>(value); };
        return workbench::ShapeRect{
            .x = f(box->x), .y = f(box->y) + m_offset, .width = f(box->width), .height = f(box->height), .radius = 0
        };
      }

      TooltipContent tooltip(workbench::ShapeId id) const override {
        return m_tips.entry(m_frame.tree, &m_frame.settings, id);
      }

      float transitionMs() const override { return kTransitionMs; }

    private:
      PartFrame m_frame;
      workbench::FlameScene m_scene;
      float m_offset = 0;
      mutable EntryTooltips m_tips;
    };
  } // namespace

  std::unique_ptr<ScenePart> makeFlamePart() { return std::make_unique<FlamePart>(); }
} // namespace luciole
