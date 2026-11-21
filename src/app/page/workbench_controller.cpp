#include "app/page/workbench_controller.h"

#include "app/page/page_context.h"
#include "app/page/preview_controller.h"
#include "app/page/workbench_open.h"
#include "app/page/workbench_preview.h"
#include "app/page/workbench_session.h"
#include "app/workspace_command.h"
#include "browser/browser_controller.h"
#include "browser/workbench_services.h"
#include "config/view_preference_writer.h"
#include "config/visit_service.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "fs/location_within.h"
#include "media/media_player.h"
#include "media/media_probe.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/browser/file_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/jump_list_view.h"
#include "ui/workbench/pane_shell.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/workbench_chrome.h"
#include "ui/workbench/workbench_preferences.h"
#include "wayland/wayland_seat.h"
#include "workbench/column_plan.h"
#include "workbench/pane_layout.h"

#include <algorithm>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <utility>

namespace {
  namespace wb = luciole::workbench;
  using luciole::Location;

  // `node` is `ancestor` or lies below it; the walk is bounded by the depth of the scene.
  bool inside(const Node* node, const Node& ancestor) {
    for (; node != nullptr; node = node->parent()) {
      if (node == &ancestor) {
        return true;
      }
    }
    return false;
  }

  // Keys the instrument, Jump ahead, the strips and the dividers take while they have focus.
  bool viewKey(const KeyboardEvent& event) {
    if ((event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0) {
      return false;
    }
    const auto sym = event.sym;
    return KeySymbol::isLeft(sym)
        || KeySymbol::isRight(sym)
        || KeySymbol::isUp(sym)
        || KeySymbol::isDown(sym)
        || KeySymbol::isHome(sym)
        || KeySymbol::isEnd(sym)
        || KeySymbol::isPageUp(sym)
        || KeySymbol::isPageDown(sym)
        || KeySymbol::isEnterOrSpace(sym)
        || KeySymbol::isBackspace(sym)
        || event.utf32 == '+'
        || event.utf32 == '='
        || event.utf32 == '-';
  }
} // namespace

WorkbenchController::WorkbenchController(
    PageContext& page, MenuController& menus, PreviewController& preview, luciole::FileViewAdapter& list,
    luciole::FileIconCache& icons, luciole::VisitService& visits, luciole::ViewPreferenceWriter& preferences,
    Hooks hooks
)
    : m_page(page), m_menus(menus), m_preview(preview), m_list(list), m_icons(icons), m_visits(visits),
      m_preferences(preferences), m_hooks(std::move(hooks)) {}

WorkbenchController::~WorkbenchController() = default;

void WorkbenchController::load(const luciole::BrowserSettings& settings) { m_settings = settings; }

void WorkbenchController::setServices(luciole::WorkbenchServices& services) { m_services = &services; }

void WorkbenchController::onFrame() {
  if (const auto& folder = m_page.browser().model().snapshot().location; folder && folder != m_visited) {
    m_trail.visit(*folder);
    m_visited = folder;
  }
  const bool wanted =
      m_page.viewMode() == luciole::ViewMode::Workbench && m_services != nullptr && m_page.fileList() != nullptr;
  if (wanted && !m_session) {
    mount();
  } else if (!wanted && m_session) {
    unmount();
    if (m_page.active() && m_page.dispatcher().focusedArea() == nullptr) {
      m_page.focusFileList(); // the focused part went with the view
    }
  }
  if (!m_session) {
    return;
  }
  const auto feed = luciole::workbenchFeed(m_page.browser());
  m_session->update(feed, luciole::workbenchChoices(m_settings, feed.showHidden));
  m_columnPreview->setPreviewSize(luciole::workbenchPreviewSize(m_settings.workbench));
  m_columnPreview->setAudioVisual(luciole::audioVisualChoice(m_settings.workbench));
  m_columnPreview->setMediaLoop(m_settings.workbench.mediaLoop);
  m_columnPreview->update();
  restAt();
}

void WorkbenchController::mount() {
  m_session = std::make_unique<luciole::WorkbenchSession>(
      m_page.filePane(), m_page.fileList()->focusArea(), m_services->measureCache(), m_icons, m_hooks.modifiers
  );
  m_session->keepPillRoom(*m_page.fileList());
  m_columnPreview = std::make_unique<luciole::WorkbenchPreview>(
      m_session->previewSlot(), m_services->previewWorkers(), luciole::MediaPlayer::available(),
      luciole::WorkbenchPreview::Hooks{
          .frameNeeded = [this] { frameNeeded(); },
          .open = [this](const luciole::DirectoryEntry& entry) { openFile(entry.location()); },
          .overlay = [this](const auto& placement, float textSize) { overlay(placement, textSize); },
          .previewSizeChosen =
              [this](int percent) {
                m_settings.workbench.previewSize = percent;
                m_preferences.submit({{luciole::BrowserPreference::WorkbenchPreviewSize, percent}});
              },
          .audioVisualChosen =
              [this](const auto& c) { m_preferences.submit(chooseAudioVisual(m_settings.workbench, c)); },
          .mediaLoopChosen = [this](bool on) { m_preferences.submit(chooseMediaLoop(m_settings.workbench, on)); },
          .modifiers = m_hooks.modifiers,
      }
  );
  m_columnPreview->setListTextSize(Style::fontSizeBody);
  m_columnPreview->setEditor(editor());
  wire();
  applyGeometry();
  auto& view = m_session->view();
  view.setTrail(&m_trail);
  view.setListTextSize(Style::fontSizeBody);
  setListIconSize(m_listIcon);
  view.setPalette(luciole::workbenchPalette(palette));
  m_paletteConn = paletteChanged().connect([this] {
    if (m_session) {
      m_session->view().setPalette(luciole::workbenchPalette(palette));
      frameNeeded();
    }
  });
}

void WorkbenchController::unmount() {
  m_paletteConn.disconnect();
  m_restTimer.stop();
  m_restDeadline.reset();
  m_pointerInList = false;
  markList(std::nullopt);
  m_preview.setTarget(std::nullopt);
  m_columnPreview.reset();
  m_session.reset();
}

void WorkbenchController::close() {
  if (m_session) {
    unmount();
  }
}

void WorkbenchController::wire() {
  auto& view = m_session->view();
  view.onGo = [this](const luciole::WorkbenchEntryRequest& request) { go(request); };
  view.onSelect = [this](const luciole::WorkbenchEntryRequest& request) { select(request); };
  view.onActivate = [this](const luciole::WorkbenchEntryRequest& request) {
    if (request.button != BTN_MIDDLE) {
      openFile(request.location);
    }
  };
  view.onContextMenu = [this](const luciole::WorkbenchContextRequest& request) { contextMenu(request); };
  view.onShowMore = [this](const luciole::WorkbenchMoreRequest& request) { showMore(request); };
  view.onInstrumentChosen = [this](luciole::WorkbenchInstrument kind) {
    m_settings.workbench.instrument = kind;
    m_preferences.submit({{luciole::BrowserPreference::WorkbenchInstrument, kind}});
  };
  view.onRibbonChosen = [this](luciole::WorkbenchRibbon kind) {
    m_settings.workbench.ribbon = kind;
    m_preferences.submit({{luciole::BrowserPreference::WorkbenchRibbon, kind}});
  };
  view.onGeometry = [this](const luciole::WorkbenchGeometry& geometry) { saveGeometry(geometry); };
  view.onPreview = [this](const std::optional<luciole::LookaheadPreview>& preview) {
    if (!preview) {
      m_columnPreview->follow(std::nullopt, false); // the slot was parked: what it showed goes now
    }
    frameNeeded();
  };
  view.onListPointed = [this](const std::optional<Location>& location) { markList(location); };
  view.onFocus = [this](InputArea* area) {
    if (area != nullptr && m_page.active()) {
      m_page.dispatcher().setFocus(area);
    }
  };
  view.onFrameNeeded = [this] { frameNeeded(); };
  m_session->model().setChangedCallback([this] { frameNeeded(); });
  m_session->lookahead().setChangedCallback([this] { frameNeeded(); });
  visitsChanged();
}

void WorkbenchController::applyGeometry() {
  auto geometry = luciole::workbenchGeometry(m_settings.workbench);
  auto& view = m_session->view();
  geometry.panes.expanded = view.shell().state().expanded; // settings keep no expanded flag
  view.setGeometry(geometry);
}

void WorkbenchController::applySettings(
    const luciole::BrowserSettings& before, const luciole::BrowserSettings& settings, bool force
) {
  const bool geometry =
      force || luciole::workbenchGeometry(before.workbench) != luciole::workbenchGeometry(settings.workbench);
  m_settings = settings;
  if (m_session && geometry) {
    applyGeometry();
  }
}

void WorkbenchController::visitsChanged() {
  if (m_session) {
    m_session->model().setVisits([&visits = m_visits](const Location& folder) { return visits.visitsUnder(folder); });
  }
}

void WorkbenchController::saveGeometry(const luciole::WorkbenchGeometry& geometry) {
  const auto after = luciole::withWorkbenchGeometry(m_settings.workbench, geometry);
  const auto edits = luciole::workbenchGeometryEdits(m_settings.workbench, after);
  m_settings.workbench = after;
  if (!edits.empty()) {
    m_preferences.submit(edits);
  }
}

void WorkbenchController::go(const luciole::WorkbenchEntryRequest& request) {
  if (request.button == BTN_MIDDLE) {
    openInNewTab(request.location, request.modifiers);
    return;
  }
  m_page.browser().open(request.location);
}

void WorkbenchController::select(const luciole::WorkbenchEntryRequest& request) {
  if (request.button == BTN_MIDDLE) {
    return;
  }
  auto& browser = m_page.browser();
  auto& model = browser.model();
  if (const auto id = luciole::rowOf(model, request.location); id && model.select(*id)) {
    const auto found = std::ranges::find(model.rows(), *id);
    if (auto* list = m_page.fileList(); list != nullptr && found != model.rows().end()) {
      list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
    }
    m_page.requestSync();
    return;
  }
  // An entry deeper than the folder: its own folder opens with it selected.
  if (const auto parent = request.location.parent()) {
    browser.open(*parent);
    m_page.revealOnReload({request.location});
  }
}

void WorkbenchController::openInNewTab(const Location& location, std::uint32_t modifiers) {
  const auto command = (modifiers & KeyMod::Shift) != 0
      ? (m_page.settings().switchToNewTab ? WorkspaceCommand::NewBackgroundTab : WorkspaceCommand::NewTab)
      : WorkspaceCommand::OpenTab;
  m_page.requestWorkspace(command, location, std::nullopt);
}

void WorkbenchController::markList(const std::optional<Location>& location) {
  const auto id = location ? luciole::rowOf(m_page.browser().model(), *location) : std::nullopt;
  const auto previous = m_list.linkedEntry();
  if (id == previous) {
    return;
  }
  m_list.setLinkedEntry(id);
  rebindRow(previous);
  rebindRow(id);
  frameNeeded();
}

void WorkbenchController::rebindRow(std::optional<luciole::EntryId> id) {
  auto* list = m_page.fileList();
  if (!id || list == nullptr) {
    return;
  }
  const auto& rows = m_page.browser().model().rows();
  if (const auto found = std::ranges::find(rows, *id); found != rows.end()) {
    list->notifyItemChanged(static_cast<std::size_t>(found - rows.begin()));
  }
}

void WorkbenchController::pointer(const PointerEvent& event, float x, float y) {
  using Type = PointerEvent::Type;
  if (event.type == Type::Motion || event.type == Type::Enter) {
    pointList(ScenePoint{.x = x, .y = y});
  } else if (event.type == Type::Leave) {
    pointList(std::nullopt);
  }
}

bool WorkbenchController::previewWheel(const PointerEvent& event, float x, float y) {
  return m_columnPreview
      && m_hooks.modifiers() == KeyMod::Ctrl
      && m_columnPreview->wheelSize(x, y, event.axis, event.axisValue120, event.axisDiscrete, event.axisValue);
}

void WorkbenchController::setListIconSize(int size) {
  m_listIcon = size;
  if (!m_session) {
    return;
  }
  auto& view = m_session->view();
  for (std::size_t slot = 0; slot < luciole::workbench::kColumnsAhead; ++slot) {
    view.column(slot).setListIconSize(size);
  }
  view.jumpList().setListIconSize(size);
}

bool WorkbenchController::overRowList(float x, float y) const {
  if (!m_session) {
    return false;
  }
  const auto over = [x, y](const Node& node) {
    for (const Node* up = &node; up != nullptr; up = up->parent()) {
      if (!up->visible()) {
        return false;
      }
    }
    return node.containsScenePoint(x, y);
  };
  const auto* list = m_page.fileList();
  if (list != nullptr && m_session->shows(*list) && over(*list)) {
    return true;
  }
  auto& view = m_session->view();
  for (std::size_t slot = 0; slot < luciole::workbench::kColumnsAhead; ++slot) {
    if (over(view.column(slot))) {
      return true;
    }
  }
  return over(view.jumpList());
}

void WorkbenchController::pointerLeft() { pointList(std::nullopt); }

bool WorkbenchController::coversFilePane() const { return m_session && !m_session->showsPane(); }

void WorkbenchController::pointList(std::optional<ScenePoint> at) {
  if (!m_session) {
    return;
  }
  auto& state = m_session->state();
  const auto serial = state.serial();
  const auto now = luciole::WorkbenchState::Clock::now();
  auto* list = m_page.fileList();
  const bool over = at && list != nullptr && m_session->shows(*list) && list->containsScenePoint(at->x, at->y);
  if (over) {
    const auto row = luciole::listEntryAt(m_page.browser().model(), *list, at->x, at->y);
    state.pointerInList(row, at->x, at->y, now);
    if (row) {
      state.hover(luciole::WorkbenchPane::List, *row);
    } else {
      state.unhover(luciole::WorkbenchPane::List);
    }
    m_pointerInList = true;
  } else if (m_pointerInList) {
    m_pointerInList = false;
    state.pointerLeft(now);
    state.unhover(luciole::WorkbenchPane::List);
  }
  restAt();
  if (state.serial() != serial) {
    frameNeeded();
  }
}

bool WorkbenchController::key(const KeyboardEvent& event) {
  if (!event.pressed || event.preedit) {
    return false;
  }
  if (luciole::takesPreviewKey(m_page.viewMode(), event.sym, event.modifiers)) {
    return true; // no docked preview to show or hide, and no preference to change
  }
  if (!m_session) {
    return false;
  }
  auto& view = m_session->view();
  auto& shell = view.shell();
  // A text field's Escape belongs to the field: it cancels the edit or closes the completions first.
  if (event.modifiers == 0 && KeySymbol::isEscape(event.sym) && shell.state().expanded && !m_hooks.editing()) {
    shell.setState(wb::withExpanded(shell.state(), false));
    frameNeeded();
    return true;
  }
  auto* focused = m_page.dispatcher().focusedArea();
  if (!inside(focused, view) || inside(focused, m_page.filePane()) || !viewKey(event)) {
    return false;
  }
  m_page.dispatcher().keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  frameNeeded();
  return true;
}

bool WorkbenchController::resizing() const { return m_session && m_session->view().resizing(); }

bool WorkbenchController::hidesDockedPreview() const { return m_page.viewMode() == luciole::ViewMode::Workbench; }

void WorkbenchController::placePreview() {
  if (!m_session) {
    return;
  }
  const auto& slot = m_session->previewSlot();
  const bool shown = m_page.visible() && m_session->shows(slot) && slot.width() > 0 && slot.height() > 0;
  // the slot's column divider keeps its presses on the slot's edge: the page's pane is drawn above the view
  const auto bounds = shown ? std::optional(
                                  m_session->previewInJump() ? luciole::jumpPreviewBounds(sceneRect(slot))
                                                             : luciole::previewBounds(sceneRect(slot))
                              )
                            : std::nullopt;
  m_columnPreview->place(
      *m_session, m_page.browser().model(), bounds, m_page.surface().renderTarget().renderer().renderScale()
  );
}

void WorkbenchController::hide() {
  if (m_columnPreview) {
    m_columnPreview->hide();
  }
}

void WorkbenchController::overlay(const std::optional<luciole::ColumnPreview::Placement>& placement, float textSize) {
  std::optional<PreviewTarget> target;
  if (placement) {
    target = PreviewTarget{
        .entry = placement->previewed.entry,
        .bounds = placement->bounds,
        .textSize = textSize,
    };
  }
  m_preview.setTarget(std::move(target));
}

void WorkbenchController::restAt() {
  const auto deadline = m_session ? m_session->state().deadline() : std::nullopt;
  if (deadline == m_restDeadline) {
    return;
  }
  m_restDeadline = deadline;
  if (!deadline) {
    m_restTimer.stop();
    return;
  }
  const auto delay = std::chrono::ceil<std::chrono::milliseconds>(*deadline - luciole::WorkbenchState::Clock::now());
  m_restTimer.start(std::max(delay, std::chrono::milliseconds{0}), [this] {
    m_restDeadline.reset();
    frameNeeded();
  });
}

void WorkbenchController::openFile(const luciole::Location& location) {
  std::vector<const luciole::DirectoryModel*> models{&m_page.browser().model()};
  if (m_session) {
    for (std::size_t column = 0; column < m_session->lookahead().count(); ++column) {
      models.push_back(&m_session->lookahead().model(column));
    }
  }
  luciole::openWorkbenchFile(
      location, models, m_page.choosing(),
      {.visited = [this](const luciole::Location& visited) { recordVisit(visited); },
       .launch = m_hooks.launch,
       .browseArchive = m_hooks.browseArchive}
  );
}

bool WorkbenchController::inDrive(const luciole::Location& root) const {
  if (m_editor && m_editor->savingIn(root)) {
    return true;
  }
  if (!m_session) {
    return false;
  }
  const auto& lookahead = m_session->lookahead();
  for (std::size_t column = 0; column < lookahead.count(); ++column) {
    if (luciole::locationWithin(lookahead.location(column), root)) {
      return true;
    }
  }
  return m_columnPreview && m_columnPreview->reading(root);
}

bool WorkbenchController::unsavedIn(const luciole::Location& root) { return m_editor && m_editor->unsavedIn(root); }

void WorkbenchController::releaseLocation(const luciole::Location& root) {
  if (!m_session) {
    return;
  }
  m_session->model().releaseLocation(root);
  m_session->lookahead().releaseLocation(root);
  if (m_columnPreview) {
    m_columnPreview->releaseLocation(root);
  }
}

bool WorkbenchController::readingDrive(const luciole::Location& root) const {
  return (m_services != nullptr && m_services->measureService().reading(root))
      || (m_session && m_session->lookahead().reading(root))
      || (m_editor && m_editor->savingIn(root))
      || luciole::MediaPlayer::stopsPending()
      || luciole::MediaProbe::readsPending()
      || (m_columnPreview && m_columnPreview->reading(root));
}

void WorkbenchController::frameNeeded() { m_page.surface().requestUpdate(); }
