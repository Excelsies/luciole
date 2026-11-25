#pragma once

#include "app/page/workbench_session.h"
#include "browser/workbench_services.h"
#include "browser/workbench_state.h"
#include "core/timer_manager.h"
#include "ui/workbench/column_preview_content.h"
#include "ui/workbench/column_preview_loads.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace luciole {
  class ColumnPreviewView;
  class PreviewEditController;

  // The file a page's column slot previews.
  struct WorkbenchPreviewTarget {
    PreviewedEntry previewed;
    std::size_t slot = 0;   // the column the slot starts at; 0 is column A
    bool rested = false;    // chosen by a resting pointer or a pin, which waited or meant it already
    int posterPixels = 512; // the poster's longest side, in buffer pixels
  };

  // The target of `state` was chosen by the pointer, which rested on it first, or by a pin.
  bool restedTarget(const WorkbenchState& state);

  // The longest side of `slot` in buffer pixels at `scale`, the size a media poster is asked for: at least 64 and
  // at most 4096.
  int posterPixels(const LayoutRect& slot, float scale);

  // The preview of a page's column slot. It picks the view for the file's kind (columnPreviewKind) and shows it in
  // the slot's ColumnPreviewView, with the file's name, kind, facts and size controls above it. Kinds the page's own
  // preview shows (Still), and a picture no decoder reads, are left to that preview, laid over the slot below its
  // header through the overlay hook. The preview size applies to every kind the slot draws but video and sound. A file
  // the cursor moves to loads once it has stayed the target for kDwell, so moving across files loads none of them; a
  // target the pointer rested on, or a pin, loads at once. A newer target cancels the load in flight, and a load shows
  // its loading state while it runs. One preview is held at a time: a new file, no file, a hidden slot and a hidden
  // page each free the one before. Main context only.
  class WorkbenchPreview {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kDwell = WorkbenchState::kDwell;

    struct Hooks {
      std::function<void()> frameNeeded;
      // The Open button: opens the file the way the page's launcher does.
      std::function<void(const DirectoryEntry&)> open;
      // The page's own preview over the slot's rectangle with all its text at `textSize`, the header's, or none.
      std::function<void(const std::optional<ColumnPreview::Placement>&, float textSize)> overlay;
      // The user chose a preview size in percent, already applied here; the owner saves it.
      std::function<void(int percent)> previewSizeChosen;
      // The user changed a sound file's visualizer with its buttons, already applied here; the owner saves it.
      std::function<void(const AudioVisualChoice&)> audioVisualChosen = nullptr;
      // The user turned a video or sound file's loop button on or off, already applied here; the owner saves it.
      std::function<void(bool)> mediaLoopChosen = nullptr;
      // The keyboard modifiers held, read when a sound file's visual button is clicked.
      std::function<std::uint32_t()> modifiers = nullptr;
      std::function<Clock::time_point()> clock = Clock::now;
    };

    // `view` outlives this object. Text needs the text service of `workers`. `playback` says the build plays video
    // and audio.
    WorkbenchPreview(ColumnPreviewView& view, WorkbenchPreviewWorkers workers, bool playback, Hooks hooks);
    ~WorkbenchPreview();
    WorkbenchPreview(const WorkbenchPreview&) = delete;
    WorkbenchPreview& operator=(const WorkbenchPreview&) = delete;
    WorkbenchPreview(WorkbenchPreview&&) = delete;
    WorkbenchPreview& operator=(WorkbenchPreview&&) = delete;

    // After layout: follows the file `session`'s columns preview in its slot, whose scene rectangle is `slot` while
    // it is shown, and moves the overlay with it. `scale` is the page's buffer scale.
    void place(WorkbenchSession& session, const DirectoryModel& list, std::optional<LayoutRect> slot, float scale);
    // The file to preview, or none; `shown` says the slot is on screen. A new file, or the same one with new facts,
    // starts over.
    void follow(const std::optional<WorkbenchPreviewTarget>& target, bool shown);
    // The page's editor, which outlives this object: it gets the edit bar of the text this preview shows and, while a
    // file is edited, the slot. It is asked before place() follows another target, keeps the target against a resting
    // pointer and while a save takes the list's cursor away, and takes the content's text size.
    void setEditor(PreviewEditController& editor);
    // The page went out of sight and draws no more frames: the load and its content go now, the file stays, and the
    // next place() with a shown slot loads it again.
    void hide();
    // Once per frame, before layout: starts a load that is due and keeps the content and its facts current.
    void update();
    // The text size of the list beside the columns; the preview's own text follows the slot's column below it.
    void setListTextSize(float size);
    // The preview size in percent, one of kPreviewSizes; another value counts as kDefaultPreviewSize. The content is
    // laid out again once; text keeps its line at the top and a picture its point at the center.
    void setPreviewSize(int percent);
    // The sound visualizer's choice, applied at once to a sound file that plays.
    void setAudioVisual(const AudioVisualChoice& choice);
    // Whether video and sound files play again from the start when they end, applied at once to one that plays.
    void setMediaLoop(bool loop);
    // The archive container types listed as archives; applies to the next file shown.
    void setOpenAsArchive(const ArchiveContainerSet& openAsArchive) { m_openAsArchive = openAsArchive; }
    // Ctrl with the wheel at scene point x, y, with the axis and values the compositor sent: a vertical turn over the
    // slot's content steps the size once per whole notch, down smaller, and the owner is told. True when taken.
    bool wheelSize(float x, float y, std::uint32_t axis, std::int32_t value120, std::int32_t discrete, double value);

    // The page's own preview is to lie over the slot.
    bool overlaid() const;
    std::optional<ColumnPreviewKind> kind() const noexcept { return m_kind; }
    int previewSize() const noexcept { return m_previewSize; }
    const AudioVisualChoice& audioVisual() const noexcept { return m_audioVisual; }
    bool mediaLoop() const noexcept { return m_mediaLoop; }
    // The text size of the content: the header's, one below the list's, times the preview size. A text editor in
    // the slot draws at this size.
    float contentTextSize() const;
    // A load waits for the dwell.
    bool waiting() const noexcept { return m_due.has_value(); }
    // The drive at `root` is being removed: a file of it that the slot shows or loads goes now.
    void releaseLocation(const Location& root);
    // The slot loads or shows a file of `root` or of a folder below it.
    bool reading(const Location& root) const;
    const ColumnPreviewLoad* load() const noexcept { return m_load.get(); }
    // Loads started since construction.
    std::uint64_t loadsStarted() const noexcept { return m_loadsStarted; }

  private:
    void start();
    void drop();
    void forget();
    // While the editor holds the slot: the facts line takes the size and date the listing gives the edited file, and
    // the lines or cells of the file as last saved.
    void followListedFacts(const std::optional<ColumnPreview::Placement>& placement);
    void schedule(bool now);
    void applyTextSize();
    void applySizeControls();
    float headerTextSize() const;
    ColumnPreviewFacts facts() const;
    void needFrame() const;

    ColumnPreviewView& m_view;
    WorkbenchPreviewWorkers m_workers;
    bool m_playback = false;
    Hooks m_hooks;
    PreviewEditController* m_editor = nullptr;
    std::optional<WorkbenchPreviewTarget> m_target;
    std::optional<ColumnPreviewKind> m_kind;
    std::unique_ptr<ColumnPreviewLoad> m_load;
    std::optional<Clock::time_point> m_due;
    Timer m_dwellTimer;
    ColumnPreviewFacts m_shownFacts;
    std::uint64_t m_loadsStarted = 0;
    float m_listTextSize;
    int m_previewSize;
    AudioVisualChoice m_audioVisual;
    bool m_mediaLoop = false;
    ArchiveContainerSet m_openAsArchive;
    float m_overlayTop = 0;      // the header's height the overlay was last placed below
    float m_overlayTextSize = 0; // the text size the overlay was last given
    bool m_shown = false;
    bool m_still = false;   // the content check sent the file back to the page's own preview
    bool m_overlay = false; // the overlay hook last showed the page's preview
  };
} // namespace luciole
