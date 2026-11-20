#pragma once

#include "browser/lookahead_columns.h"
#include "browser/workbench_state.h"
#include "model/directory_options.h"
#include "render/scene/node.h"
#include "ui/workbench/kept_snapshots.h"
#include "ui/workbench/snapshot_places.h"
#include "workbench/color.h"
#include "workbench/column_plan.h"
#include "workbench/measure.h"
#include "workbench/pane_layout.h"
#include "workbench/preview_home.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Button;
class InputArea;
class Segmented;

namespace luciole {
  class FileIconCache;
  class InstrumentView;
  class JumpListView;
  class JumpPane;
  class PaneShell;
  class QuietColumnView;
  class RibbonView;
  class FolderTrail;
  class WorkbenchCenterView;
  class WorkbenchColumns;
  class WorkbenchHeaders;
  class WorkbenchModel;
  class WorkbenchParts;

  // The Workbench choices from settings.
  struct WorkbenchChoices {
    WorkbenchInstrument instrument = WorkbenchInstrument::Nest;
    WorkbenchRibbon ribbon = WorkbenchRibbon::Flame;
    bool strips = true;
    workbench::LayoutSettings layout; // size by, color by, emphasis and hidden entries
    ActivationMode activation = ActivationMode::DoubleClick;
    WorkbenchPreviewPlace previewPlace =
        WorkbenchPreviewPlace::JumpAheadWhenNarrow; // where a preview goes with no column
    bool operator==(const WorkbenchChoices&) const = default;
  };

  // Pane sizes, collapsed and expanded flags, and the column widths. `panes.columnWidths` is not read and is
  // reported empty: `columns` holds the widths.
  struct WorkbenchGeometry {
    workbench::PaneState panes;
    workbench::ColumnWidths columns;
    std::optional<double> strips; // the ancestor strips pane's width; none is the default
    bool operator==(const WorkbenchGeometry&) const = default;
  };

  // An entry a click or a key asked for. `button` is the pointer button of a click, 0 for a key or a pane that
  // reports none; the owner opens a middle click in a new tab. `modifiers` are the keyboard modifiers held.
  struct WorkbenchEntryRequest {
    Location location;
    std::uint32_t button = 0;
    std::uint32_t modifiers = 0;
  };

  // A merged piece ("+N more") asks to list the entries it stands for: at the pointer for a click, at the piece for a
  // key.
  struct WorkbenchMoreRequest {
    WorkbenchPane pane = WorkbenchPane::Instrument;
    Location folder;                 // whose entries the piece merges
    std::vector<MergedEntry> listed; // heaviest first, at most WorkbenchView::kMoreListed
    std::size_t count = 0;           // every entry the piece stands for
    float sceneX = 0;
    float sceneY = 0;
  };

  // A context menu asked for over a pane; no entry for a pane's background.
  struct WorkbenchContextRequest {
    WorkbenchPane pane = WorkbenchPane::Instrument;
    std::optional<Location> entry;
    float sceneX = 0;
    float sceneY = 0;
  };

  // The Workbench view of one folder: the instrument over Jump ahead, the ribbon over the center with its ancestor
  // strips, the owner's list and the columns ahead. It links hover across panes through the page state, switches
  // kinds from the pane headers, expands the instrument, and decides what the columns ahead follow. It reads the
  // models it is given in update(), once per frame, pushes only what changed, and reports what only the page can do
  // through its callbacks. It does no I/O.
  class WorkbenchView final : public Node {
  public:
    using Clock = WorkbenchState::Clock;
    static constexpr std::size_t kMoreListed = 100;

    // `modifiers` reports the keyboard modifiers held; `clock` gives the time pointer rests are measured with.
    WorkbenchView(
        FileIconCache& icons, const std::function<std::uint32_t()>& modifiers,
        std::function<Clock::time_point()> clock = Clock::now
    );
    ~WorkbenchView() override;
    WorkbenchView(const WorkbenchView&) = delete;
    WorkbenchView& operator=(const WorkbenchView&) = delete;
    WorkbenchView(WorkbenchView&&) = delete;
    WorkbenchView& operator=(WorkbenchView&&) = delete;

    // Sources, borrowed until replaced; null for none. Set them to null before destroying them.
    void setModel(const WorkbenchModel* model);
    void setLookahead(LookaheadColumns* lookahead);
    // The view resets it whenever the folder changes and keeps the list cursor and the expanded flag in it.
    void setState(WorkbenchState* state);
    // The children last visited, which the columns ahead follow when nothing else steers them and the Iris hub
    // offers as the way back down.
    void setTrail(const FolderTrail* trail);

    // The page's folder; a snapshot of another folder shows as loading.
    void setFolder(const Location& folder);
    // The list's cursor entry; call it every frame.
    void setCursor(std::optional<WorkbenchEntry> entry);
    // Kinds change at the next update.
    void setChoices(const WorkbenchChoices& choices);
    // Never reported back.
    void setGeometry(const WorkbenchGeometry& geometry);
    void setListTextSize(float size);
    void setPalette(const workbench::ColorPalette& palette);
    // The list region and the area that takes its keys; returns the list it replaces.
    std::unique_ptr<Node> setList(std::unique_ptr<Node> list, InputArea* focus);
    // Shown in the slot of the file the columns ahead stop at; returns the preview it replaces.
    std::unique_ptr<Node> setPreview(std::unique_ptr<Node> preview);

    // Reads the sources and pushes what changed. The owner calls it once per frame, before layout.
    void update();

    WorkbenchGeometry geometry() const;
    const WorkbenchChoices& choices() const noexcept { return m_choices; }
    // A divider is being dragged.
    bool resizing() const;
    // Where the preview of the file the columns ahead stop at is shown: in its column, in the place of Jump ahead
    // when no column has room and the choices say so, or nowhere.
    workbench::PreviewHome previewHome() const;

    PaneShell& shell() const noexcept { return *m_shell; }
    WorkbenchCenterView& center() const noexcept { return *m_center; }
    InstrumentView& instrument() const noexcept { return *m_instrument; }
    RibbonView& ribbon() const noexcept { return *m_ribbon; }
    JumpListView& jumpList() const noexcept { return *m_jump; }
    QuietColumnView& column(std::size_t slot) const;
    Segmented& instrumentSwitch() const;
    Segmented& ribbonSwitch() const;
    // Reset layout in the instrument header (shown while it is expanded) and in the ribbon header.
    Button& resetButton(workbench::Pane pane) const;

    std::function<void(const WorkbenchEntryRequest&)> onGo;       // enter a folder
    std::function<void(const WorkbenchEntryRequest&)> onSelect;   // put the list's cursor on an entry, maybe deeper
    std::function<void(const WorkbenchEntryRequest&)> onActivate; // open a file
    std::function<void(const WorkbenchContextRequest&)> onContextMenu;
    std::function<void(const WorkbenchMoreRequest&)> onShowMore;
    std::function<void(WorkbenchInstrument)> onInstrumentChosen;
    std::function<void(WorkbenchRibbon)> onRibbonChosen;
    std::function<void(const WorkbenchGeometry&)> onGeometry; // committed: save it
    // The file the columns ahead preview, and its slot; none when they preview nothing.
    std::function<void(const std::optional<LookaheadPreview>&)> onPreview;
    // The entry of the folder to mark in the list because another pane points at it or below it.
    std::function<void(const std::optional<Location>&)> onListPointed;
    // Keyboard focus belongs here now: the instrument when it expands, the list when it returns or a strip was
    // clicked, and the nearest strip shown, else the list, once a strip's key has changed the folder.
    std::function<void(InputArea*)> onFocus;
    std::function<void()> onFrameNeeded;

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doLayout(Renderer& renderer) override;

  private:
    enum class Focus : std::uint8_t { None, Instrument, List };

    void wireJump();
    void wireCenter();
    void applyChoices();
    void applyFolder();
    bool readModel();
    void syncExpanded();
    // Points each part at the hovered entry and its cursor, in the nodes of the snapshot it shows.
    void linkParts();
    void link();
    // True when a part shows another snapshot afterwards.
    bool pushParts();
    // The pane points at `entry`, or at nothing.
    void hover(WorkbenchPane pane, const std::optional<WorkbenchEntry>& entry);
    void unhover(WorkbenchPane pane);

    void resetLayout();
    // The geometry changed for good: the owner saves it.
    void committed();

    std::function<Clock::time_point()> m_clock;
    std::function<std::uint32_t()> m_modifiers;
    PaneShell* m_shell = nullptr;
    InstrumentView* m_instrument = nullptr;
    JumpPane* m_jumpPane = nullptr;
    JumpListView* m_jump = nullptr;
    RibbonView* m_ribbon = nullptr;
    WorkbenchCenterView* m_center = nullptr;
    std::unique_ptr<WorkbenchHeaders> m_headers;
    std::unique_ptr<WorkbenchColumns> m_columns;
    std::unique_ptr<WorkbenchParts> m_parts;

    const WorkbenchModel* m_model = nullptr;
    LookaheadColumns* m_lookahead = nullptr;
    WorkbenchState* m_state = nullptr;
    std::optional<Location> m_folder;
    std::optional<WorkbenchEntry> m_cursor;
    WorkbenchChoices m_choices;
    workbench::ColorPalette m_palette;
    workbench::PartLimits m_limits;
    InputArea* m_listFocus = nullptr;

    PlacedSnapshot m_placed; // the current folder's snapshot; empty while it loads
    std::optional<Location> m_listPointed;
    std::uint64_t m_seenModel = 0;
    std::uint64_t m_seenState = 0;
    std::uint64_t m_jumpSerial = 0;
    workbench::NodeId m_jumpLinked = workbench::kNoNode;
    std::optional<bool> m_expanded; // as last seen in the shell; none before the first update
    Focus m_focus = Focus::None;
    bool m_titledPreview = false; // the Jump ahead header reads "Preview"
    bool m_folderDirty = false;
    bool m_modelDirty = true;
    bool m_choicesDirty = true;
    bool m_linkDirty = true;
    bool m_jumpDirty = true;
    bool m_settingsChanged = true;
  };
} // namespace luciole
