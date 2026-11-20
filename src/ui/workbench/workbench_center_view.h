#pragma once

#include "fs/location.h"
#include "render/animation/animation_manager.h"
#include "ui/controls/flex.h"
#include "ui/workbench/ancestor_strips.h"
#include "workbench/column_plan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class PaneDivider;

namespace luciole {
  // Which way the folder changed, for the motion that shows it.
  enum class CenterMotion : std::uint8_t { Deeper, Up, Across };

  // Deeper when `to` lies below `from`, Up when `from` lies below `to`, else Across.
  CenterMotion centerMotion(const Location& from, const Location& to);

  // The center of a Workbench page: the ancestor strips pane, the list, and up to three columns ahead, each column
  // with a divider on its edge toward the list and the strips pane with one on its edge beside the list. Widths come
  // from workbench::planColumns: the strips pane keeps its width whatever the folder's depth, the list keeps its
  // minimum, columns drop from the far end when the room runs out and come back when it returns, and a column drag
  // works from the widths at its start and holds the number of columns it started with. Widths are reported to the
  // owner once a drag ends, a key steps a divider or a divider resets. A change of folder slides and fades the
  // strips, the list and the columns into their new places.
  class WorkbenchCenterView final : public Flex {
  public:
    static constexpr float kMotionMs = 240;     // with strips
    static constexpr float kFlatMotionMs = 200; // without
    static constexpr float kMotionSlide = 40;   // px the content travels toward its place

    WorkbenchCenterView();

    // The list region, owned by the view; returns the one it replaces, or null.
    std::unique_ptr<Node> setList(std::unique_ptr<Node> list);
    // What column slot 0, 1 or 2 shows, owned by the view; returns the node it replaces, or null. A slot past the
    // third changes nothing and hands `content` back.
    std::unique_ptr<Node> setColumn(std::size_t slot, std::unique_ptr<Node> content);
    // From this slot to the far edge, the slot's node takes the room of every planned column (a preview), and the
    // later slots and their dividers are hidden. None gives every column its own slot.
    void setSpanFrom(std::optional<std::size_t> slot);
    // The page's folder: the strips stand for the folders above it, and a change of folder plays the motion.
    void setFolder(const Location& folder);
    // Nearest first; strips planned at the last layout's width are planned again at once.
    void setAncestors(std::vector<AncestorEntry> ancestors);
    // Marks the strip that holds `folder`; none, or a folder no strip stands for, marks nothing.
    void setPointedFolder(const std::optional<Location>& folder);
    // From where the motion puts them, the strips, the list and the columns ahead below a preview's slot move and
    // fade into the places of the last layout. Only translation and opacity change, so nothing is laid out while
    // they move. Nothing moves while the toolkit's motion is off, before the first layout or while the view is
    // hidden.
    void beginMotion(CenterMotion motion);
    // Ends a motion at once while the view is hidden; nothing otherwise. The owner calls it once per frame.
    void settle();
    // Without strips the list starts at the near edge and takes their width.
    void setStripsShown(bool shown);
    // Normalized by workbench::normalizeWidths; never reported back. Ignored while a divider is dragged.
    void setWidths(const workbench::ColumnWidths& widths);
    // The strips pane's saved width, normalized by workbench::normalizeStripsWidth; none is the default. Never
    // reported back. Ignored while a divider is dragged.
    void setStripsWidth(std::optional<double> width);
    // Also called when the strips pane's width changes; stripsWidth() gives it.
    void setOnWidths(std::function<void(const workbench::ColumnWidths&)> callback);

    Node* list() const noexcept { return m_list; }
    Node* column(std::size_t slot) const;
    AncestorStrips& strips() const noexcept { return *m_strips; }
    // The folders setFolder gave the strips, nearest first; a strip's id is its index here.
    const std::vector<Location>& ancestors() const noexcept { return m_ancestors; }
    bool moving() const noexcept { return m_motion != 0; }
    // Once a strip's key has changed the folder while a strip keeps the keyboard focus: the nearest strip shown, or
    // null when none is, so the list takes the focus. None otherwise; the focus going elsewhere drops the request.
    std::optional<InputArea*> takeStripFocus();

    // A strip asked for its folder, with the pointer button of a click or 0 for a key.
    std::function<void(const Location& folder, std::uint32_t button)> onStripActivate;
    // The hovered strip's folder; none off the strips and over a stack.
    std::function<void(const std::optional<Location>& folder)> onStripHover;
    // The divider on column 0, 1 or 2; a larger index reads the last.
    PaneDivider& divider(std::size_t column) const;
    const workbench::ColumnWidths& widths() const noexcept { return m_widths; }
    // The strips pane's saved width; a narrow view draws the pane narrower without changing it.
    std::optional<double> stripsWidth() const { return m_stripsEdge->width(); }
    // The divider on the strips pane's edge beside the list; hidden without strips.
    PaneDivider& stripsDivider() const { return m_stripsEdge->divider(); }
    bool stripsShown() const noexcept { return m_stripsShown; }
    // The plan of the last layout: strips, list and the columns drawn.
    const workbench::ColumnPlan& plan() const noexcept { return m_plan; }
    bool resizing() const;

  private:
    // Where a drag of one divider starts: the plan it was last placed by, and that plan's column count.
    struct DragBase {
      workbench::ColumnDrag drag;
      std::size_t columns = 0;
    };

    struct Inputs {
      double width = -1;
      std::size_t ancestors = 0;
      bool strips = true;
      workbench::ColumnWidths widths;
      std::optional<double> stripsWidth;
      std::optional<std::size_t> held;
      bool operator==(const Inputs&) const = default;
    };

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;
    void doLayout(Renderer& renderer) override;
    void replan();
    void showMotion(float progress);
    void stopMotion();
    bool shownInScene() const;
    void buildDivider(std::size_t column);
    void placeColumns(Renderer& renderer);
    void placeDividers(Renderer& renderer);
    LayoutRect sceneRect(double x, double extent) const;
    void dragTo(std::size_t column, float value, bool finished);
    void step(std::size_t column, float value);
    void reset(std::size_t column);
    void cancel();
    void commit(const workbench::ColumnWidths& widths);

    AncestorStrips* m_strips = nullptr;
    std::optional<Location> m_folder;
    std::vector<Location> m_ancestors;
    std::optional<Location> m_stripKeyFrom; // the folder a strip's key asked to leave
    Node* m_list = nullptr;
    std::array<Node*, workbench::kColumnsAhead> m_columns{};
    std::array<PaneDivider*, workbench::kColumnsAhead> m_dividers{};
    std::size_t m_ancestorCount = 0;
    bool m_stripsShown = true;
    std::optional<std::size_t> m_spanFrom;
    workbench::ColumnWidths m_widths;
    std::optional<workbench::ColumnDrag> m_drag;
    std::optional<std::size_t> m_held; // the column count a drag holds
    std::array<std::optional<DragBase>, workbench::kColumnsAhead> m_dragBases{};
    std::unique_ptr<StripsPaneEdge> m_stripsEdge;
    std::function<void(const workbench::ColumnWidths&)> m_onWidths;
    workbench::ColumnPlan m_plan;
    Inputs m_planned;
    AnimationManager::Id m_motion = 0;
    std::uint64_t m_motionRun = 0; // a completion collected for an older motion does nothing
    float m_slide = 0;
  };
} // namespace luciole
