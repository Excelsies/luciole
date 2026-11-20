#include "ui/workbench/workbench_view.h"

#include "browser/workbench_model.h"
#include "ui/workbench/instrument_view.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/jump_pane.h"
#include "ui/workbench/kept_snapshots.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/ribbon_view.h"
#include "ui/workbench/workbench_center_view.h"
#include "ui/workbench/workbench_chrome.h"
#include "ui/workbench/workbench_columns.h"
#include "ui/workbench/workbench_parts.h"

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    constexpr std::array<WorkbenchPane, 7> kCoveredPanes{
        WorkbenchPane::JumpAhead, WorkbenchPane::Ribbon,  WorkbenchPane::Strips,  WorkbenchPane::List,
        WorkbenchPane::ColumnA,   WorkbenchPane::ColumnB, WorkbenchPane::ColumnC,
    };

    template <class Callback, class... Args> void fire(const Callback& callback, Args&&... args) {
      if (callback) {
        callback(std::forward<Args>(args)...);
      }
    }

    template <class View> View* mount(PaneShell& shell, PaneRegion region, std::unique_ptr<View> view) {
      auto* raw = view.get();
      (void)shell.setContent(region, paneBody(std::move(view)));
      return raw;
    }
  } // namespace

  WorkbenchView::WorkbenchView(
      FileIconCache& icons, const std::function<std::uint32_t()>& modifiers, std::function<Clock::time_point()> clock
  )
      : m_clock(clock ? std::move(clock) : std::function<Clock::time_point()>(&Clock::now)), m_modifiers(modifiers) {
    m_shell = static_cast<PaneShell*>(addChild(std::make_unique<PaneShell>()));
    m_instrument =
        mount(*m_shell, PaneRegion::Instrument, std::make_unique<InstrumentView>(m_choices.instrument, modifiers));
    auto jump = std::make_unique<JumpPane>();
    m_jumpPane = jump.get();
    m_jump = &jump->list();
    (void)m_shell->setContent(PaneRegion::Jump, std::move(jump));
    m_ribbon = mount(*m_shell, PaneRegion::Ribbon, std::make_unique<RibbonView>(m_choices.ribbon, modifiers));
    m_center = static_cast<WorkbenchCenterView*>(
        m_shell->setContent(PaneRegion::Center, std::make_unique<WorkbenchCenterView>())
    );
    m_columns = std::make_unique<WorkbenchColumns>(
        *m_center, *m_jumpPane, icons, modifiers, m_clock,
        WorkbenchColumnsCallbacks{
            .go = [this](const WorkbenchEntryRequest& folder) { fire(onGo, folder); },
            .activate = [this](const WorkbenchEntryRequest& file) { fire(onActivate, file); },
            .context =
                [this](WorkbenchPane pane, const Location& entry, float x, float y) {
                  fire(onContextMenu, WorkbenchContextRequest{.pane = pane, .entry = entry, .sceneX = x, .sceneY = y});
                },
            .preview = [this](const std::optional<LookaheadPreview>& preview) { fire(onPreview, preview); },
            .frameNeeded = [this] { fire(onFrameNeeded); },
        }
    );
    m_headers = std::make_unique<WorkbenchHeaders>(*m_shell, [this] { resetLayout(); });
    const auto choose = [this](auto& chosen, auto kind, const auto& report) {
      if (kind != chosen) {
        chosen = kind;
        m_choicesDirty = true;
        fire(report, kind);
        fire(onFrameNeeded);
      }
    };
    m_headers->onInstrument = [this, choose](WorkbenchInstrument kind) {
      choose(m_choices.instrument, kind, onInstrumentChosen);
    };
    m_headers->onRibbon = [this, choose](WorkbenchRibbon kind) { choose(m_choices.ribbon, kind, onRibbonChosen); };
    m_parts = std::make_unique<WorkbenchParts>(
        *m_instrument, *m_ribbon,
        WorkbenchPartsCallbacks{
            .hover = [this](WorkbenchPane pane, const std::optional<WorkbenchEntry>& entry) { hover(pane, entry); },
            .go = [this](const WorkbenchEntryRequest& folder) { fire(onGo, folder); },
            .select = [this](const WorkbenchEntryRequest& entry) { fire(onSelect, entry); },
            .activate = [this](const WorkbenchEntryRequest& file) { fire(onActivate, file); },
            .context = [this](const WorkbenchContextRequest& request) { fire(onContextMenu, request); },
            .more = [this](const WorkbenchMoreRequest& request) { fire(onShowMore, request); },
            .frameNeeded = [this] { fire(onFrameNeeded); },
        }
    );
    wireJump();
    wireCenter();
    m_shell->setOnCommit([this](const wb::PaneState&) { committed(); });
    m_center->setOnWidths([this](const wb::ColumnWidths&) { committed(); });
  }

  WorkbenchView::~WorkbenchView() {
    // The columns let go of their views before the center that holds them goes.
    m_columns.reset();
  }

  void WorkbenchView::wireJump() {
    m_jump->onHover = [this](const wb::JumpRow* row) {
      auto location = row != nullptr ? jumpLocation(m_placed, row->id) : std::nullopt;
      hover(WorkbenchPane::JumpAhead, location ? std::optional(WorkbenchEntry{*location, true}) : std::nullopt);
    };
    const auto jump = [this](const wb::JumpRow& row, std::uint32_t button) {
      if (auto location = jumpLocation(m_placed, row.id)) {
        const auto modifiers = button != 0 ? m_modifiers() : 0;
        fire(onGo, WorkbenchEntryRequest{.location = std::move(*location), .button = button, .modifiers = modifiers});
      }
    };
    m_jump->onActivate = [jump](const wb::JumpRow& row) { jump(row, 0); };
    m_jump->onMiddleClick = [jump](const wb::JumpRow& row) { jump(row, BTN_MIDDLE); };
  }

  void WorkbenchView::wireCenter() {
    m_center->onStripHover = [this](const std::optional<Location>& folder) {
      hover(WorkbenchPane::Strips, folder ? std::optional(WorkbenchEntry{*folder, true}) : std::nullopt);
    };
    m_center->onStripActivate = [this](const Location& folder, std::uint32_t button) {
      if (button != 0) { // the press took the focus to the strip
        m_focus = Focus::List;
      }
      fire(
          onGo,
          WorkbenchEntryRequest{.location = folder, .button = button, .modifiers = button != 0 ? m_modifiers() : 0}
      );
    };
  }

  void WorkbenchView::setModel(const WorkbenchModel* model) {
    m_model = model;
    m_modelDirty = true;
  }

  void WorkbenchView::setLookahead(LookaheadColumns* lookahead) {
    m_lookahead = lookahead;
    m_columns->setSources(m_lookahead, m_state);
  }

  void WorkbenchView::setState(WorkbenchState* state) {
    m_state = state;
    if (m_state != nullptr) {
      m_state->setCursor(m_cursor);
    }
    m_columns->setSources(m_lookahead, m_state);
    m_linkDirty = true;
    m_expanded.reset(); // the new state learns the expanded flag at the next update
  }

  void WorkbenchView::setTrail(const FolderTrail* trail) {
    m_columns->setTrail(trail);
    m_parts->setTrail(trail);
  }

  void WorkbenchView::setFolder(const Location& folder) {
    if (m_folder != folder) {
      m_folder = folder;
      m_folderDirty = true;
    }
  }

  void WorkbenchView::setCursor(std::optional<WorkbenchEntry> entry) {
    if (entry == m_cursor) {
      return;
    }
    m_cursor = std::move(entry);
    m_linkDirty = true;
    if (m_state != nullptr) {
      m_state->setCursor(m_cursor);
    }
  }

  void WorkbenchView::setChoices(const WorkbenchChoices& choices) {
    if (choices != m_choices) {
      m_choices = choices;
      m_choicesDirty = true;
    }
  }

  void WorkbenchView::setGeometry(const WorkbenchGeometry& geometry) {
    m_shell->setState(geometry.panes);
    m_center->setWidths(geometry.columns);
    m_center->setStripsWidth(geometry.strips);
  }

  void WorkbenchView::setListTextSize(float size) { m_columns->setListTextSize(size); }

  void WorkbenchView::setPalette(const wb::ColorPalette& palette) { m_palette = palette; }

  std::unique_ptr<Node> WorkbenchView::setList(std::unique_ptr<Node> list, InputArea* focus) {
    m_listFocus = focus;
    return m_center->setList(std::move(list));
  }

  std::unique_ptr<Node> WorkbenchView::setPreview(std::unique_ptr<Node> preview) {
    return m_columns->setPreview(std::move(preview));
  }

  QuietColumnView& WorkbenchView::column(std::size_t slot) const { return m_columns->view(slot); }

  Segmented& WorkbenchView::instrumentSwitch() const { return m_headers->instrumentSwitch(); }

  Segmented& WorkbenchView::ribbonSwitch() const { return m_headers->ribbonSwitch(); }

  Button& WorkbenchView::resetButton(wb::Pane pane) const {
    return m_headers->resetButton(pane == wb::Pane::Instrument);
  }

  bool WorkbenchView::resizing() const { return m_shell->resizing() || m_center->resizing(); }

  WorkbenchGeometry WorkbenchView::geometry() const {
    WorkbenchGeometry geometry{
        .panes = m_shell->state(), .columns = m_center->widths(), .strips = m_center->stripsWidth()
    };
    geometry.panes.columnWidths.reset();
    return geometry;
  }

  void WorkbenchView::update() {
    if (m_choicesDirty) {
      applyChoices();
    }
    if (m_folderDirty) {
      applyFolder();
    }
    m_center->settle(); // hidden, as under the expanded instrument, it stops its motion
    const bool snapshotChanged = readModel();
    syncExpanded();
    if (m_state != nullptr) {
      m_state->advance(m_clock());
    }
    const auto stateSerial = m_state != nullptr ? m_state->serial() : 0;
    if (snapshotChanged || m_linkDirty || stateSerial != m_seenState) {
      link();
      m_linkDirty = false;
      m_seenState = stateSerial;
    }
    const bool settingsChanged = std::exchange(m_settingsChanged, false);
    m_columns->setPreviewPlace(m_choices.previewPlace, m_shell->jumpOpen());
    m_columns->update(m_placed, m_choices.layout, snapshotChanged || settingsChanged);
    if (pushParts()) {
      linkParts(); // marks name the nodes of the snapshot each part shows
    }
    if (const auto strip = m_center->takeStripFocus()) {
      fire(onFocus, *strip != nullptr ? *strip : m_listFocus);
    }
    const auto focus = std::exchange(m_focus, Focus::None);
    if (focus == Focus::Instrument) {
      fire(onFocus, &m_instrument->shapeView().inputArea());
    } else if (focus == Focus::List && m_listFocus != nullptr) {
      fire(onFocus, m_listFocus);
    }
  }

  void WorkbenchView::applyChoices() {
    m_choicesDirty = false;
    if (m_instrument->kind() != m_choices.instrument) {
      m_instrument->setKind(m_choices.instrument);
      unhover(WorkbenchPane::Instrument); // the old scene's pieces are gone
    }
    if (m_ribbon->kind() != m_choices.ribbon) {
      m_ribbon->setKind(m_choices.ribbon);
      unhover(WorkbenchPane::Ribbon);
    }
    m_headers->show(m_choices.instrument, m_choices.ribbon);
    m_shell->setRibbonKind(static_cast<wb::RibbonKind>(m_choices.ribbon));
    m_center->setStripsShown(m_choices.strips);
    m_jump->setActivationMode(m_choices.activation);
    m_columns->setActivation(m_choices.activation);
    if (m_choices.layout != m_parts->settings()) {
      m_parts->setSettings(m_choices.layout);
      m_settingsChanged = true;
    }
  }

  void WorkbenchView::applyFolder() {
    m_folderDirty = false;
    m_modelDirty = true;
    m_linkDirty = true;
    if (m_folder) {
      m_center->setFolder(*m_folder);
    }
    if (m_state != nullptr) {
      m_state->reset();
    }
  }

  bool WorkbenchView::readModel() {
    const auto serial = m_model != nullptr ? m_model->serial() : 0;
    if (!m_modelDirty && serial == m_seenModel) {
      return false;
    }
    m_modelDirty = false;
    m_seenModel = serial;
    auto snapshot = m_model != nullptr ? m_model->snapshot() : nullptr;
    if (snapshot && (!m_folder || snapshot->location != *m_folder)) {
      snapshot.reset(); // still the previous folder's picture
    }
    if (snapshot == m_placed.snapshot) {
      return false;
    }
    m_placed = placeSnapshot(snapshot);
    m_parts->setSnapshot(m_placed);
    m_jumpDirty = true;
    return true;
  }

  void WorkbenchView::syncExpanded() {
    const bool expanded = m_shell->state().expanded;
    if (m_expanded == expanded) {
      return;
    }
    if (m_expanded.has_value() || expanded) {
      m_focus = expanded ? Focus::Instrument : Focus::List;
    }
    m_expanded = expanded;
    m_headers->setExpanded(expanded);
    if (m_state == nullptr) {
      return;
    }
    m_state->setExpanded(expanded);
    if (!expanded) {
      return;
    }
    for (const auto pane : kCoveredPanes) {
      m_state->unhover(pane); // covered panes get no leave events
    }
  }

  void WorkbenchView::linkParts() { m_parts->mark(m_state != nullptr ? m_state->hovered() : std::nullopt, m_cursor); }

  void WorkbenchView::link() {
    linkParts();
    const auto& hovered = m_state != nullptr ? m_state->hovered() : std::nullopt;
    const bool jumpLinks = hovered && hovered->pane != WorkbenchPane::JumpAhead;
    const auto linked = jumpLinks ? nodeOf(m_placed, hovered->entry.location) : wb::kNoNode;
    if (m_jumpDirty || linked != m_jumpLinked) {
      m_jumpDirty = false;
      m_jumpLinked = linked;
      JumpListContent content{
          .list = m_placed.snapshot ? m_placed.snapshot->jump : wb::JumpList{},
          .linked = linked,
          .notMeasured = m_placed.snapshot && m_placed.snapshot->notMeasured
      };
      m_jump->setContent(std::move(content), ++m_jumpSerial);
    }

    const bool stripLinks = hovered && hovered->pane != WorkbenchPane::Strips;
    m_center->setPointedFolder(stripLinks ? std::optional(hovered->entry.location) : std::nullopt);

    std::optional<Location> listed;
    if (hovered && hovered->pane != WorkbenchPane::List && m_folder) {
      listed = entryWithin(*m_folder, hovered->entry.location);
    }
    if (listed != m_listPointed) {
      m_listPointed = std::move(listed);
      fire(onListPointed, m_listPointed);
    }
  }

  bool WorkbenchView::pushParts() {
    const bool moved = m_parts->push(m_palette, resizing());
    // the ladder's limits grow with the depth of the snapshot it was just given
    const wb::PartLimits limits{.instrument = InstrumentView::limits(), .ribbon = m_ribbon->limits()};
    if (!(limits == m_limits)) {
      m_limits = limits;
      m_shell->setLimits(limits);
    }
    return moved;
  }

  void WorkbenchView::hover(WorkbenchPane pane, const std::optional<WorkbenchEntry>& entry) {
    if (m_state == nullptr) {
      return;
    }
    if (entry) {
      m_state->hover(pane, *entry);
    } else {
      m_state->unhover(pane);
    }
    fire(onFrameNeeded);
  }

  void WorkbenchView::unhover(WorkbenchPane pane) {
    if (m_state != nullptr) {
      m_state->unhover(pane);
    }
  }

  void WorkbenchView::resetLayout() {
    m_shell->setState({});
    m_center->setWidths({});
    m_center->setStripsWidth(std::nullopt);
    committed();
  }

  void WorkbenchView::committed() {
    fire(onGeometry, geometry());
    fire(onFrameNeeded);
  }

  LayoutSize WorkbenchView::doMeasure(Renderer&, const LayoutConstraints& constraints) {
    return constraints.constrain(
        {constraints.hasMaxWidth ? constraints.maxWidth : 0, constraints.hasMaxHeight ? constraints.maxHeight : 0}
    );
  }

  void WorkbenchView::doLayout(Renderer& renderer) {
    const LayoutRect rect{0, 0, width(), height()};
    m_shell->arrange(renderer, rect);
    // the drawn columns and the Jump ahead pane come out of this layout: the preview moves to its home before the
    // frame is drawn, and the panes are arranged again with it there
    m_columns->setPreviewPlace(m_choices.previewPlace, m_shell->jumpOpen());
    const bool moved = m_columns->placePreview();
    const bool titledPreview = m_columns->previewHome() == wb::PreviewHome::JumpAhead;
    if (moved || titledPreview != m_titledPreview) {
      m_titledPreview = titledPreview;
      m_shell->setTitle(wb::Pane::Jump, titledPreview ? "Preview" : "Jump ahead");
      m_shell->arrange(renderer, rect);
    }
  }

  wb::PreviewHome WorkbenchView::previewHome() const { return m_columns->previewHome(); }
} // namespace luciole
