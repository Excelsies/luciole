#pragma once

#include "render/scene/node.h"
#include "workbench/column_plan.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Box;
class Glyph;
class InputArea;
class Label;
class PaneDivider;

namespace luciole {
  // A folder above the current one; `id` is what the strips report for it.
  struct AncestorEntry {
    std::string name;
    std::string path;
    std::uint64_t id = 0;
  };

  // The folders above the current one as thin vertical bands, each named: the nearest beside the list, each farther
  // one no wider than the one nearer. Widths and folding come from a workbench strip plan, and a stack strip, the
  // "+N levels" band at the far side, stands for every level it folds. The bands sit against the node's edge beside
  // the list, and the room beyond the farthest stays empty. A click, a middle click, Enter or Space asks for a
  // strip's folder, or for the nearest folder of a stack; the tooltip gives that folder's path. A strip the plan no
  // longer shows takes no clicks or keys.
  class AncestorStrips final : public Node {
  public:
    static constexpr std::size_t kMaxStrips = workbench::kMaxStrips;

    AncestorStrips();

    // Nearest first.
    void setAncestors(std::vector<AncestorEntry> ancestors);
    // Farthest first, as workbench::ancestorStrips gives them; a strip for a level past the ancestors is not shown.
    void setPlan(std::vector<workbench::Strip> plan);
    // The system font's size over its default: the paddings and icons grow with it, as the plan's bands do.
    void setTextScale(float scale);
    // The folder another pane points at: the strip that holds it is marked. None, or an id that is no ancestor,
    // marks nothing.
    void setPointed(std::optional<std::uint64_t> id);

    // Strips shown, farthest first.
    std::size_t count() const noexcept { return m_shown; }
    // A shown strip's input area; null past count().
    InputArea* area(std::size_t index) const;
    // The input area of the shown strip beside the list; null when none is shown.
    InputArea* nearest() const;
    // A strip, shown or not, has the keyboard focus.
    bool focused() const;
    // The text written along a shown strip: its folder's name, or "N levels" on a stack. Null on a strip too narrow
    // or too short for text, and past count().
    const Label* label(std::size_t index) const;

    // The hovered strip's folder; none when the pointer leaves a strip or is over a stack. When new ancestors give
    // the strip under the pointer another folder, the next motion reports it.
    std::function<void(std::optional<std::uint64_t>)> onHover;
    // The folder a strip stands for, with the pointer button of the click, or 0 for a key.
    std::function<void(std::uint64_t id, std::uint32_t button)> onActivate;

  private:
    struct Slot {
      InputArea* area = nullptr;
      Box* background = nullptr;
      Glyph* icon = nullptr;
      Label* name = nullptr;
      Label* count = nullptr;
      workbench::Strip strip;
      std::size_t ancestor = 0; // index of the folder a click goes to
    };

    void doLayout(Renderer& renderer) override;
    Slot& createSlot();
    void bind();
    void bindSlot(Slot& slot, const workbench::Strip& strip);
    void placeSlot(Renderer& renderer, const Slot& slot, float x) const;
    void applyLook(const Slot& slot) const;
    bool pointedAt(const Slot& slot) const;
    std::size_t lastAncestor(const Slot& slot) const;
    std::optional<std::uint64_t> folderOf(const Slot& slot) const;
    void hover(const Slot& slot);
    void activate(const Slot& slot, std::uint32_t button) const;

    std::vector<AncestorEntry> m_ancestors;
    std::vector<workbench::Strip> m_plan;
    std::vector<Slot> m_slots;
    std::size_t m_shown = 0;
    std::optional<std::uint64_t> m_pointed;
    std::optional<std::uint64_t> m_hoverSent; // the last folder onHover reported
    float m_textScale = 1;
  };

  // The strips pane's edge beside the list: a divider that resizes the pane, and the pane's saved width. The divider
  // is a child of the owner's node. A drag works from the width drawn at its press and is held between the narrowest
  // pane and the widest the plan allows; Left and Right step it, Escape goes back to the width it started from, and
  // Home or a double press goes back to the default.
  class StripsPaneEdge {
  public:
    // `changed` runs when a drag moves or is abandoned, `committed` once it ends, a key steps or the divider resets.
    StripsPaneEdge(Node& owner, std::int32_t zIndex, std::function<void()> changed, std::function<void()> committed);
    ~StripsPaneEdge();
    StripsPaneEdge(const StripsPaneEdge&) = delete;
    StripsPaneEdge& operator=(const StripsPaneEdge&) = delete;
    StripsPaneEdge(StripsPaneEdge&&) = delete;
    StripsPaneEdge& operator=(StripsPaneEdge&&) = delete;

    // Normalized by workbench::normalizeStripsWidth; none is the default.
    void setWidth(std::optional<double> width);
    std::optional<double> width() const noexcept { return m_width; }
    PaneDivider& divider() const noexcept { return *m_divider; }

    // Puts the divider at `rect`, the pane's edge in `plan` in the owner's coordinates; hidden when the plan has no
    // strips pane.
    void place(Renderer& renderer, const workbench::ColumnPlan& plan, const LayoutRect& rect);

  private:
    void resize(float value, bool finished);

    PaneDivider* m_divider = nullptr;
    std::function<void()> m_changed;
    std::function<void()> m_committed;
    std::optional<double> m_width;
    bool m_previewed = false;     // a drag has moved since its press: its end is not a key step
    workbench::StripsDrag m_drag; // as at the last placement, or at the press while the divider is dragged
  };
} // namespace luciole
