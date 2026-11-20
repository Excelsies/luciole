#pragma once

#include "browser/browser_controller.h"
#include "browser/lookahead_columns.h"
#include "browser/workbench_model.h"
#include "browser/workbench_state.h"
#include "render/scene/node.h"
#include "ui/controls/context_menu.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/workbench_view.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class InputArea;
class VirtualGridView;

namespace luciole {
  class FileIconCache;
  class MeasureCache;

  // What the page's browser shows, as the Workbench reads it each frame.
  struct WorkbenchFeed {
    std::optional<Location> folder;
    std::optional<WorkbenchEntry> cursor; // the entry under the list's cursor
    SortOptions sort;
    bool showHidden = false;
    bool showBackups = true;
  };

  // The folder `browser` lists, the entry under its cursor and its listing options.
  WorkbenchFeed workbenchFeed(const BrowserController& browser);

  // The row of `model` that lists `location`; none when it lists no such entry.
  std::optional<EntryId> rowOf(const DirectoryModel& model, const Location& location);

  // When `model` has no cursor and lists `file`, the cursor goes to the file and it joins the selection, as after a
  // save that replaced the file and took the cursor away with its old entry. True when the cursor moved.
  bool restoreCursor(DirectoryModel& model, const Location& file);

  // The entry of `model` that `list` shows at scene point `x`, `y`; none beside its rows.
  std::optional<WorkbenchEntry> listEntryAt(const DirectoryModel& model, const VirtualGridView& list, float x, float y);

  struct PreviewedEntry {
    DirectoryEntry entry;
    std::string typeLabel;
  };

  // The entry `preview` names: a row of `list` for slot 0, else a row of the column ahead before its slot. None while
  // that listing does not hold it.
  std::optional<PreviewedEntry>
  previewedEntry(const LookaheadColumns& lookahead, const DirectoryModel& list, const LookaheadPreview& preview);

  // The rows of the list a merged piece opens: a heading with the count and the folder's name, each listed entry
  // with its value, then a row for the entries left unlisted. An entry's row id is its index in `listed` plus one.
  std::vector<ContextMenuControlEntry> moreMenuEntries(const WorkbenchMoreRequest& request);

  // What choosing an entry from a merged piece's list does: a folder is entered, a file takes the list's cursor, as a
  // click on its own piece does.
  enum class MoreAction : std::uint8_t { Go, Select };

  struct MoreChoice {
    MoreAction action = MoreAction::Go;
    WorkbenchEntryRequest request;
  };

  // The choice of the moreMenuEntries row `id` made from `listed`; none for a row that names no listed entry.
  std::optional<MoreChoice> moreChoice(const std::vector<MergedEntry>& listed, int id);

  // In `mode` the page takes the docked preview's key and does nothing with it: Workbench shows no docked preview.
  bool takesPreviewKey(ViewMode mode, std::uint32_t sym, std::uint32_t modifiers);

  // The part of a column slot's scene rectangle the page's preview pane takes: all of it but the half of the
  // column divider's hit area that lies over the slot's edge toward the list, so the divider keeps its presses.
  LayoutRect previewBounds(const LayoutRect& slot);

  // The part of the preview's scene rectangle in the place of Jump ahead that the page's preview pane takes: all of it
  // but the half of the left column's divider hit area that lies over the pane's edge toward the center.
  LayoutRect jumpPreviewBounds(const LayoutRect& slot);

  // The file a column previews and the scene rectangle of its slot, which the page's preview pane shows.
  class ColumnPreview {
  public:
    struct Placement {
      PreviewedEntry previewed;
      LayoutRect bounds;
    };

    // Follows the file `lookahead` previews into `slot`, the slot's scene rectangle while it is shown. The entry is
    // looked up again whenever the columns or `list` changed, so it carries the listing's latest facts. True when
    // placement() changed and the pane must take it.
    bool update(const LookaheadColumns& lookahead, const DirectoryModel& list, std::optional<LayoutRect> slot);
    // None while no file is previewed in a shown slot, or while its listing does not hold it.
    const std::optional<Placement>& placement() const noexcept { return m_placement; }

  private:
    std::optional<Placement> m_placement;
    std::uint64_t m_serial = 0;     // of the columns at the last lookup
    std::uint64_t m_generation = 0; // of `list` at the last lookup
  };

  // The Workbench of one page while its mode is on: the view, its page model, columns ahead and state. Constructing it
  // moves the page's file pane into the view's list region and puts the view where the pane was; destroying it puts
  // the pane back in its place and frees the view and the models. Walks already started finish in the shared cache.
  // The page's browser is only read. Main context only.
  class WorkbenchSession {
  public:
    // `pane` must have a parent; `listFocus` is the area inside it that takes the list's keys. `modifiers` reports
    // the keyboard modifiers held.
    WorkbenchSession(
        Node& pane, InputArea* listFocus, MeasureCache& cache, FileIconCache& icons,
        const std::function<std::uint32_t()>& modifiers, WorkbenchModelOptions options = {}
    );
    ~WorkbenchSession();
    WorkbenchSession(const WorkbenchSession&) = delete;
    WorkbenchSession& operator=(const WorkbenchSession&) = delete;
    WorkbenchSession(WorkbenchSession&&) = delete;
    WorkbenchSession& operator=(WorkbenchSession&&) = delete;

    // Pushes what changed since the last call to the models and the view, then updates the view. Once per frame,
    // before layout.
    void update(const WorkbenchFeed& feed, const WorkbenchChoices& choices);
    // Puts room for the page's status pill at the bottom of the pane, below `list`, which lies in the pane, so the
    // pill covers no row. The room is the pane's, so a drop there finds the pane, and a wheel turned there scrolls
    // `list`. Once per session; the session takes the room out again.
    void keepPillRoom(VirtualGridView& list);

    WorkbenchView& view() const noexcept { return *m_view; }
    // `node` lies in the view, and it and every node above it up to the view are visible.
    bool shows(const Node& node) const;
    // The page's file pane is shown. False while the expanded instrument covers it: the pane keeps its geometry
    // then, so it must take no pointer input.
    bool showsPane() const { return shows(*m_pane); }
    // The pane's place in `frame`'s coordinates while it is shown, where the page's status pill floats; none
    // otherwise. A change of folder slides the pane, not its place.
    std::optional<LayoutRect> paneArea(const Node& frame) const;
    // Where the view shows a previewed file: empty until the page's column preview fills it.
    ColumnPreviewView& previewSlot() const noexcept { return *m_slot; }
    // The preview is shown in the place of Jump ahead rather than in a column; the page then places its own preview
    // over the slot's rectangle through jumpPreviewBounds().
    bool previewInJump() const { return m_view->previewHome() == workbench::PreviewHome::JumpAhead; }
    ColumnPreview& columnPreview() noexcept { return m_columnPreview; }
    WorkbenchModel& model() noexcept { return m_model; }
    LookaheadColumns& lookahead() noexcept { return m_lookahead; }
    const LookaheadColumns& lookahead() const noexcept { return m_lookahead; }
    WorkbenchState& state() noexcept { return m_state; }

  private:
    WorkbenchModel m_model;
    LookaheadColumns m_lookahead;
    WorkbenchState m_state;
    ColumnPreview m_columnPreview;
    Node* m_parent = nullptr;
    Node* m_pane = nullptr;
    Node* m_room = nullptr; // in the pane, below the list
    WorkbenchView* m_view = nullptr;
    ColumnPreviewView* m_slot = nullptr;
    std::optional<WorkbenchFeed> m_feed;
    std::optional<workbench::LayoutSettings> m_layout;
  };
} // namespace luciole
