#include "app/page/workbench_preview.h"

#include "app/page/preview_edit_controller.h"
#include "config/settings.h"
#include "ui/style.h"
#include "ui/workbench/column_preview_view.h"
#include "ui/workbench/list_rows.h"
#include "ui/workbench/workbench_preferences.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace luciole {
  namespace {
    constexpr int kSmallestPoster = 64;
    constexpr int kLargestPoster = 4096;
    constexpr auto kFitted = "Video and sound are always fitted to the pane.";
    constexpr auto kOneSize = "This preview is shown at one size.";

    // The same file with the same facts; names are compared before the locations, which cost more.
    bool sameFile(const DirectoryEntry& a, const DirectoryEntry& b) {
      return a.name == b.name
          && a.type == b.type
          && a.size == b.size
          && a.modifiedSeconds == b.modifiedSeconds
          && a.contentType == b.contentType
          && a.location() == b.location();
    }
  } // namespace

  bool restedTarget(const WorkbenchState& state) {
    return state.targetSource() == WorkbenchTargetSource::Pointer || state.pinned(0) || state.pinned(1);
  }

  int posterPixels(const LayoutRect& slot, float scale) {
    const float side = std::max(slot.width, slot.height) * scale;
    if (!std::isfinite(side)) {
      return kSmallestPoster;
    }
    return static_cast<int>(std::clamp(std::ceil(side), float{kSmallestPoster}, float{kLargestPoster}));
  }

  WorkbenchPreview::WorkbenchPreview(
      ColumnPreviewView& view, WorkbenchPreviewWorkers workers, bool playback, Hooks hooks
  )
      : m_view(view), m_workers(workers), m_playback(playback), m_hooks(std::move(hooks)),
        m_listTextSize(Style::fontSizeBody), m_previewSize(kDefaultPreviewSize) {
    if (!m_hooks.clock) {
      m_hooks.clock = Clock::now;
    }
    m_view.onPreviewSizeChosen = [this](int percent) {
      setPreviewSize(percent);
      if (m_hooks.previewSizeChosen) {
        m_hooks.previewSizeChosen(m_previewSize);
      }
    };
    m_view.onOpen = [this] {
      if (m_target && m_hooks.open) {
        m_hooks.open(m_target->previewed.entry);
      }
    };
    m_view.clear();
    applyTextSize();
    applySizeControls();
  }

  WorkbenchPreview::~WorkbenchPreview() {
    if (m_editor != nullptr) {
      m_editor->detach();
    }
    drop();
    m_view.onOpen = {};
    m_view.onPreviewSizeChosen = {};
    m_view.clear();
  }

  void WorkbenchPreview::place(
      WorkbenchSession& session, const DirectoryModel& list, std::optional<LayoutRect> slot, float scale
  ) {
    auto& column = session.columnPreview();
    auto& state = session.state();
    const bool moved = column.update(session.lookahead(), list, slot);
    const auto& placement = column.placement();
    const auto& preview = session.lookahead().preview();
    std::optional<WorkbenchPreviewTarget> target;
    if (placement) {
      target = WorkbenchPreviewTarget{
          .previewed = placement->previewed,
          .slot = preview ? preview->slot : 0,
          .rested = restedTarget(state),
          .posterPixels = posterPixels(placement->bounds, scale),
      };
    } else if (!slot && m_target && preview && preview->file == m_target->previewed.entry.location()) {
      target = m_target; // the slot is hidden: the file stays, its content goes
    }
    const auto aimed = preview ? std::optional(preview->file) : std::nullopt;
    const bool kept = m_editor != nullptr
        && !m_editor->follow(
            aimed, placement ? &placement->previewed.entry : nullptr, slot.has_value(), state.expanded()
        );
    state.setFrozen(m_editor != nullptr && m_editor->gate().editing());
    state.holdCursor(m_editor != nullptr && m_editor->holdsCursor());
    if (kept) {
      followListedFacts(placement); // the editor holds the slot
      return;
    }
    follow(target, placement.has_value());
    const bool overlay = placement && overlaid();
    const float top = overlay ? std::min(m_view.headerHeight(), placement->bounds.height) : 0;
    const float textSize = headerTextSize();
    if (overlay != m_overlay || (overlay && (moved || top != m_overlayTop || textSize != m_overlayTextSize))) {
      m_overlay = overlay;
      m_overlayTop = top;
      m_overlayTextSize = textSize;
      if (m_hooks.overlay) {
        auto below = overlay ? placement : std::nullopt; // the slot's header with its size controls stays in sight
        if (below) {
          below->bounds.y += top;
          below->bounds.height -= top;
        }
        m_hooks.overlay(below, textSize);
      }
    }
  }

  void WorkbenchPreview::follow(const std::optional<WorkbenchPreviewTarget>& target, bool shown) {
    if (!target) {
      if (m_target) {
        forget();
      }
      m_shown = shown;
      return;
    }
    if (m_target && sameFile(m_target->previewed.entry, target->previewed.entry)) {
      m_target->posterPixels = target->posterPixels;
      if (target->slot != m_target->slot) {
        m_target->slot = target->slot;
        applyTextSize();
      }
      if (shown != m_shown) {
        m_shown = shown;
        if (!shown) {
          drop(); // a hidden slot holds nothing; the file loads again once it shows
          needFrame();
        } else if (m_kind != ColumnPreviewKind::Still && !m_still) {
          schedule(true);
        }
      }
      return;
    }
    drop();
    m_target = target;
    m_shown = shown;
    m_still = false;
    const auto& entry = target->previewed.entry;
    m_kind = columnPreviewKind(
        entry.location(), entry.contentType.value_or(""), entry.type == FileType::Regular, m_playback
    );
    m_view.setFile(entry.displayName(), target->previewed.typeLabel);
    m_shownFacts = facts();
    m_view.setFacts(columnPreviewFactsText(m_shownFacts));
    m_view.showMessage({});
    applyTextSize();
    applySizeControls();
    if (*m_kind != ColumnPreviewKind::Still && shown) {
      schedule(target->rested);
    }
    needFrame();
  }

  void WorkbenchPreview::setEditor(PreviewEditController& editor) {
    m_editor = &editor;
    editor.attach(m_view, {.release = [this] { drop(); }, .closed = [this] { forget(); }});
  }

  void WorkbenchPreview::followListedFacts(const std::optional<ColumnPreview::Placement>& placement) {
    if (!m_target) {
      return;
    }
    auto facts = m_shownFacts;
    if (placement) {
      const auto& entry = placement->previewed.entry;
      // The facts first: comparing locations costs more, and they match on most frames.
      if ((entry.size != facts.bytes || entry.modifiedSeconds != facts.modified)
          && entry.name == m_target->previewed.entry.name
          && entry.location() == m_target->previewed.entry.location()) {
        m_target->previewed.entry = entry;
        facts.bytes = entry.size;
        facts.modified = entry.modifiedSeconds;
      }
    }
    // The lines or cells of the file as saved, not of the text being edited.
    if (const auto& saved = m_editor->savedShape()) {
      facts.lines = saved->lines;
      facts.cells = saved->cells;
    }
    if (facts != m_shownFacts) {
      m_shownFacts = std::move(facts);
      m_view.setFacts(columnPreviewFactsText(m_shownFacts));
    }
  }

  void WorkbenchPreview::hide() {
    m_shown = false;
    drop();
    if (m_editor != nullptr) {
      m_editor->setShown(false);
    }
  }

  void WorkbenchPreview::update() {
    if (m_editor != nullptr) {
      const bool offered = m_load && m_shown && m_target && (m_load->showsText() || m_load->showsTable());
      m_editor->offer(offered ? &m_target->previewed.entry : nullptr);
      m_editor->update(contentTextSize());
    }
    if (m_due && m_shown && m_hooks.clock() >= *m_due) {
      start();
    }
    if (!m_load) {
      return;
    }
    m_load->update();
    if (m_load->status() == ColumnPreviewStatus::Still) {
      m_still = true;
      drop();
      applySizeControls();
      needFrame();
      return;
    }
    if (const auto now = facts(); now != m_shownFacts) {
      m_shownFacts = now;
      m_view.setFacts(columnPreviewFactsText(now));
    }
  }

  void WorkbenchPreview::setListTextSize(float size) {
    m_listTextSize = size;
    applyTextSize();
  }

  void WorkbenchPreview::setPreviewSize(int percent) {
    const int size = steppedPreviewSize(percent, 0);
    if (size == m_previewSize) {
      return;
    }
    m_previewSize = size;
    m_view.setPreviewSize(size);
    if (m_load) {
      m_load->setSize(headerTextSize(), static_cast<float>(size) / 100);
    }
    needFrame();
  }

  void WorkbenchPreview::setAudioVisual(const AudioVisualChoice& choice) {
    if (choice == m_audioVisual) {
      return;
    }
    m_audioVisual = choice;
    if (m_load) {
      m_load->setAudioVisual(choice);
    }
  }

  void WorkbenchPreview::setMediaLoop(bool loop) {
    if (loop == m_mediaLoop) {
      return;
    }
    m_mediaLoop = loop;
    if (m_load) {
      m_load->setMediaLoop(loop);
    }
  }

  bool WorkbenchPreview::wheelSize(
      float x, float y, std::uint32_t axis, std::int32_t value120, std::int32_t discrete, double value
  ) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }
    const double notches = value120 != 0 ? value120 / 120.0
        : discrete != 0                  ? static_cast<double>(discrete)
                                         : value / 15.0;
    return m_view.wheelSize(x, y, notches);
  }

  float WorkbenchPreview::contentTextSize() const { return headerTextSize() * static_cast<float>(m_previewSize) / 100; }

  bool WorkbenchPreview::overlaid() const { return m_kind && (*m_kind == ColumnPreviewKind::Still || m_still); }

  void WorkbenchPreview::start() {
    m_due.reset();
    m_dwellTimer.stop();
    if (!m_target || !m_kind) {
      return;
    }
    m_load = startColumnPreviewLoad(
        m_view,
        {.entry = m_target->previewed.entry,
         .kind = *m_kind,
         .textSize = headerTextSize(),
         .scale = static_cast<float>(m_previewSize) / 100,
         .posterPixels = m_target->posterPixels,
         .audioVisual = m_audioVisual,
         .audioVisualChosen =
             [this](const AudioVisualChoice& choice) {
               m_audioVisual = choice;
               if (m_hooks.audioVisualChosen) {
                 m_hooks.audioVisualChosen(choice);
               }
             },
         .mediaLoop = m_mediaLoop,
         .mediaLoopChosen =
             [this](bool on) {
               m_mediaLoop = on;
               if (m_hooks.mediaLoopChosen) {
                 m_hooks.mediaLoopChosen(on);
               }
             },
         .modifiers = m_hooks.modifiers},
        m_workers, [this] { needFrame(); }
    );
    ++m_loadsStarted;
  }

  void WorkbenchPreview::drop() {
    m_due.reset();
    m_dwellTimer.stop();
    m_load.reset();
    if (m_editor != nullptr) {
      m_editor->offer(nullptr); // the edit bar goes with the text it was offered for
    }
  }

  void WorkbenchPreview::forget() {
    drop();
    m_target.reset();
    m_kind.reset();
    m_still = false;
    m_shownFacts = {};
    m_view.clear();
    needFrame();
  }

  void WorkbenchPreview::schedule(bool now) {
    const auto at = m_hooks.clock();
    m_due = now ? at : at + kDwell;
    if (!now) {
      m_dwellTimer.start(kDwell, [this] { needFrame(); });
    }
    needFrame();
  }

  float WorkbenchPreview::headerTextSize() const {
    return quietTextSize(m_listTextSize, m_target ? m_target->slot : 0);
  }

  void WorkbenchPreview::applyTextSize() {
    const auto size = headerTextSize();
    m_view.setTextSize(size);
    if (m_load) {
      m_load->setSize(size, static_cast<float>(m_previewSize) / 100);
    }
  }

  void WorkbenchPreview::applySizeControls() {
    m_view.setPreviewSize(m_previewSize);
    if (!m_target || !m_kind) {
      m_view.setSizeAvailable(false);
    } else if (*m_kind == ColumnPreviewKind::Media) {
      m_view.setSizeAvailable(false, kFitted);
    } else if (*m_kind == ColumnPreviewKind::Still || m_still) {
      m_view.setSizeAvailable(false, kOneSize);
    } else {
      m_view.setSizeAvailable(true);
    }
  }

  ColumnPreviewFacts WorkbenchPreview::facts() const {
    ColumnPreviewFacts out = m_load ? m_load->facts() : ColumnPreviewFacts{};
    if (m_target) {
      out.bytes = m_target->previewed.entry.size;
      out.modified = m_target->previewed.entry.modifiedSeconds;
    }
    return out;
  }

  void WorkbenchPreview::needFrame() const {
    if (m_hooks.frameNeeded) {
      m_hooks.frameNeeded();
    }
  }
} // namespace luciole
