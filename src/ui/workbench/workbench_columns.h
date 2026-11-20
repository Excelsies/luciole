#pragma once

#include "browser/lookahead_columns.h"
#include "browser/workbench_state.h"
#include "model/directory_options.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/workbench/snapshot_places.h"
#include "workbench/column_plan.h"
#include "workbench/measure.h"
#include "workbench/preview_home.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Node;

namespace luciole {
  class JumpPane;
  class QuietColumnView;
  class WorkbenchCenterView;
  struct QuietRowEvent;
  struct WorkbenchEntryRequest;

  // Per folder, the child last visited below it, so the columns ahead reopen where the user was. It keeps at most
  // kFolders folders and forgets the one recorded longest ago first.
  class FolderTrail {
  public:
    static constexpr std::size_t kFolders = 256;
    static constexpr std::size_t kDepth = 64; // ancestors one visit records

    // Each ancestor of `folder`, up to kDepth of them, remembers the child on the way to it.
    void visit(const Location& folder);
    // The child `folder` remembers, then the one that child remembers, and so on: at most `levels`.
    std::vector<Location> trail(const Location& folder, std::size_t levels) const;
    std::size_t size() const noexcept { return m_children.size(); }
    // Changes whenever a folder remembers another child.
    std::uint64_t serial() const noexcept { return m_serial; }

  private:
    struct Child {
      Location location;
      std::uint64_t stamp = 0; // when it was last recorded
    };

    void remember(const Location& folder, const Location& child);

    std::unordered_map<std::string, Child> m_children; // by the folder's URI
    std::uint64_t m_stamp = 0;
    std::uint64_t m_serial = 0;
  };

  // What the columns ahead ask of their owner.
  struct WorkbenchColumnsCallbacks {
    std::function<void(const WorkbenchEntryRequest&)> go;       // enter a folder
    std::function<void(const WorkbenchEntryRequest&)> activate; // open a file
    std::function<void(WorkbenchPane pane, const Location& entry, float sceneX, float sceneY)> context;
    std::function<void(const std::optional<LookaheadPreview>&)> preview; // the file previewed changed
    std::function<void()> frameNeeded;
  };

  // The columns ahead of a Workbench center: a quiet column view in each slot, or the owner's preview node in the
  // slot where the columns stop at a file. Columns follow the page state's target, its pins, and with no target the
  // chain rule of workbench::columnChain, which follows the folder trail where it has one. Rows report the pointer to
  // the page state; clicks follow the activation setting: a single click activates, or with double click a click pins
  // the row and a double click activates it. A middle click on a folder row asks to go there with that button.
  class WorkbenchColumns {
  public:
    using Clock = WorkbenchState::Clock;

    WorkbenchColumns(
        WorkbenchCenterView& center, JumpPane& jump, FileIconCache& icons,
        const std::function<std::uint32_t()>& modifiers, std::function<Clock::time_point()> clock,
        WorkbenchColumnsCallbacks callbacks
    );
    ~WorkbenchColumns();
    WorkbenchColumns(const WorkbenchColumns&) = delete;
    WorkbenchColumns& operator=(const WorkbenchColumns&) = delete;
    WorkbenchColumns(WorkbenchColumns&&) = delete;
    WorkbenchColumns& operator=(WorkbenchColumns&&) = delete;

    // Borrowed until replaced; null for none. Columns let go of every listing at once.
    void setSources(LookaheadColumns* lookahead, WorkbenchState* state);
    // Borrowed until replaced; null for none.
    void setTrail(const FolderTrail* trail);
    // The owner's preview, shown in the slot of a previewed file; returns the one it replaces.
    std::unique_ptr<Node> setPreview(std::unique_ptr<Node> preview);
    void setListTextSize(float size);
    void setActivation(ActivationMode mode) { m_activation = mode; }
    // What a preview with no drawn column does and whether the Jump ahead pane is shown, as of the last layout.
    void setPreviewPlace(WorkbenchPreviewPlace choice, bool jumpOpen);
    // Moves the preview to the home the choice and the last layout give it, and reports whether it moved. The view
    // calls it after laying out its panes.
    bool placePreview();
    // Where the preview is shown now.
    workbench::PreviewHome previewHome() const noexcept { return m_home; }

    // Follows the state, pushes what changed to the columns and moves the preview. `changed` says the snapshot or
    // the settings changed since the last call. Never call it from a column's own callback.
    void update(const PlacedSnapshot& snapshot, const workbench::LayoutSettings& settings, bool changed);

    QuietColumnView& view(std::size_t slot) const;

  private:
    struct Column {
      std::unordered_map<std::string, EntryId> ids;   // the listing's rows by name
      std::unordered_map<std::string, double> shares; // entries the snapshot measures, by name
    };

    void follow(const PlacedSnapshot& snapshot, const workbench::LayoutSettings& settings);
    void index(const PlacedSnapshot& snapshot, const workbench::LayoutSettings& settings);
    void push();
    // Puts the preview back in `m_preview` from wherever it is shown.
    void takeOut();
    // What the column after `slot` lists, or the file it previews.
    std::optional<WorkbenchEntry> nextOf(std::size_t slot) const;
    std::optional<EntryId> idOf(std::size_t slot, const std::optional<WorkbenchEntry>& entry) const;
    std::optional<WorkbenchEntry> entryAt(std::size_t slot, EntryId id) const;
    void open(const WorkbenchEntry& entry, const QuietRowEvent& event) const;
    void wire(std::size_t slot);
    void clear();

    WorkbenchCenterView& m_center;
    JumpPane& m_jump;
    std::function<Clock::time_point()> m_clock;
    WorkbenchColumnsCallbacks m_callbacks;
    std::array<QuietColumnView*, workbench::kColumnsAhead> m_views{};
    std::array<Column, workbench::kColumnsAhead> m_columns;
    std::unique_ptr<Node> m_parked;  // the quiet view the preview took the place of
    std::unique_ptr<Node> m_preview; // the preview while it waits outside the center
    Node* m_previewNode = nullptr;
    std::optional<std::size_t> m_previewSlot; // while the preview is in a column
    workbench::PreviewHome m_home = workbench::PreviewHome::None;
    WorkbenchPreviewPlace m_place = WorkbenchPreviewPlace::JumpAheadWhenNarrow;
    bool m_jumpOpen = false;
    std::optional<LookaheadPreview> m_reported;
    LookaheadColumns* m_lookahead = nullptr;
    WorkbenchState* m_state = nullptr;
    const FolderTrail* m_trail = nullptr;
    ActivationMode m_activation = ActivationMode::DoubleClick;
    std::uint64_t m_seenLookahead = 0;
    std::uint64_t m_seenState = 0;
    std::uint64_t m_seenTrail = 0;
    std::uint64_t m_rowsSerial = 0;
    std::uint64_t m_serial = 0;
    bool m_fresh = true;  // sources changed: follow and push at the next update
    bool m_ready = false; // the folder is measured: no column ahead then means no folders ahead
  };
} // namespace luciole
