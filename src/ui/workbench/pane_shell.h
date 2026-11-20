#pragma once

#include "ui/controls/flex.h"
#include "workbench/pane_layout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class Button;
class PaneDivider;
class PaneHeader;

namespace luciole {
  enum class PaneRegion : std::uint8_t { Instrument, Jump, Ribbon, Center };

  // The Workbench view's panes: the instrument over Jump ahead on the left, the ribbon over the center on the
  // right, with a header on every pane but the center and dividers between them. Rectangles come from
  // workbench::layoutPanes; drags, keys, collapsing and expanding change the state live and report it to the
  // owner once each change is finished.
  class PaneShell final : public Flex {
  public:
    // Allocations of one layout pass whose state, size, limits or ribbon changed: those of workbench::layoutPanes
    // and two for each header title measured at a new width. An unchanged pass makes none.
    static constexpr std::size_t kLayoutAllocationBound = 12;

    PaneShell();

    // The node below a region's header, owned by the shell; an empty pointer removes it.
    Node* setContent(PaneRegion region, std::unique_ptr<Node> content);
    // Sanitized; never reported back. A drag in progress continues from it.
    void setState(const workbench::PaneState& state);
    // The parts' limits; panes the view cannot hold close until it grows.
    void setLimits(const workbench::PartLimits& limits);
    void setRibbonKind(workbench::RibbonKind kind);
    // The title of a pane's header, which shows the longest form that fits whole: the title with the reason the view
    // closed the pane, the title alone, then the pane's name, which alone may end in an ellipsis. A shortened title
    // has the whole one as its tooltip.
    void setTitle(workbench::Pane pane, std::string_view title);
    // Called once a drag is released, a key steps a divider, a divider resets, or a pane collapses, opens,
    // expands or returns.
    void setOnCommit(std::function<void(const workbench::PaneState&)> callback);

    Node* content(PaneRegion region) const;
    PaneHeader& header(workbench::Pane pane) const;
    PaneDivider& divider(workbench::DividerId id) const;
    // The rail's button for the instrument or Jump ahead, shown while both are closed.
    Button* railButton(workbench::Pane pane) const;
    const workbench::PaneState& state() const noexcept { return m_state; }
    bool resizing() const;
    // Jump ahead is shown in the last layout: not collapsed, not closed for lack of room, not under the expanded
    // instrument. False before the first layout.
    bool jumpOpen() const;

  private:
    struct Slot {
      Node* root = nullptr;
      PaneHeader* header = nullptr;
      Node* content = nullptr;
    };
    struct DragStart {
      workbench::DividerId id = workbench::DividerId::Left;
      workbench::PaneState state;
    };
    enum class TitleForm : std::uint8_t { Whole, Bare, Name };
    struct Title {
      std::string text;        // as the owner set it
      std::string_view reason; // why the view closed the pane; empty while it is not closed by force
      std::string whole;       // the text and the reason
      float room = -1;         // the label width `form` was chosen for; below zero to choose again
      TitleForm form = TitleForm::Whole;
    };

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;
    void doLayout(Renderer& renderer) override;
    void buildDivider(workbench::DividerId id);
    void buildRail();
    void placeSlot(Renderer& renderer, PaneRegion region, const std::optional<workbench::PaneRect>& rect, bool open);
    void placeDividers(Renderer& renderer);
    void syncChrome();
    void composeTitle(std::size_t index);
    // Gives the header of slot `index`, arranged in `rect`, the longest form of its title that fits.
    void fitTitle(Renderer& renderer, std::size_t index, const LayoutRect& rect);
    std::string_view titleText(std::size_t index) const;
    LayoutRect sceneRect(const workbench::PaneRect& rect) const;
    void dragTo(workbench::DividerId id, float value, bool finished);
    void commit(const workbench::PaneState& state);

    std::array<Slot, 4> m_slots;
    std::array<Title, 3> m_titles; // of the panes with a header
    std::array<PaneDivider*, 3> m_dividers{};
    std::array<Button*, 2> m_railButtons{};
    std::array<std::string_view, 2> m_railReasons{}; // what the rail buttons say, empty while not forced
    Flex* m_rail = nullptr;
    workbench::PaneState m_state;
    workbench::PartLimits m_limits;
    workbench::RibbonKind m_kind = workbench::RibbonKind::Flame;
    std::optional<DragStart> m_drag;
    std::function<void(const workbench::PaneState&)> m_onCommit;
    workbench::PaneLayout m_layout;
    workbench::ViewSize m_layoutView;
    workbench::PartLimits m_layoutLimits;
    workbench::RibbonKind m_layoutKind = workbench::RibbonKind::Flame;
    bool m_layoutValid = false;
  };
} // namespace luciole
