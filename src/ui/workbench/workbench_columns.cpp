#include "ui/workbench/workbench_columns.h"

#include "fs/file_metadata.h"
#include "ui/workbench/jump_pane.h"
#include "ui/workbench/quiet_column_view.h"
#include "ui/workbench/workbench_center_view.h"
#include "ui/workbench/workbench_view.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    WorkbenchPane paneOf(std::size_t slot) {
      switch (slot) {
      case 0:
        return WorkbenchPane::ColumnA;
      case 1:
        return WorkbenchPane::ColumnB;
      default:
        return WorkbenchPane::ColumnC;
      }
    }
  } // namespace

  void FolderTrail::visit(const Location& folder) {
    auto child = folder;
    auto parent = folder.parent();
    for (std::size_t level = 0; level < kDepth && parent; ++level) {
      remember(*parent, child);
      child = std::move(*parent);
      parent = child.parent();
    }
  }

  void FolderTrail::remember(const Location& folder, const Location& child) {
    ++m_stamp;
    const auto [at, added] = m_children.try_emplace(folder.uri(), Child{.location = child, .stamp = m_stamp});
    if (!added) {
      at->second.stamp = m_stamp;
      if (at->second.location != child) {
        at->second.location = child;
        ++m_serial;
      }
      return;
    }
    ++m_serial;
    if (m_children.size() > kFolders) {
      m_children.erase(std::ranges::min_element(m_children, {}, [](const auto& kept) { return kept.second.stamp; }));
    }
  }

  std::vector<Location> FolderTrail::trail(const Location& folder, std::size_t levels) const {
    std::vector<Location> out;
    auto at = folder;
    for (std::size_t level = 0; level < levels; ++level) {
      const auto found = m_children.find(at.uri());
      if (found == m_children.end()) {
        break;
      }
      out.push_back(found->second.location);
      at = found->second.location;
    }
    return out;
  }

  WorkbenchColumns::WorkbenchColumns(
      WorkbenchCenterView& center, JumpPane& jump, FileIconCache& icons,
      const std::function<std::uint32_t()>& modifiers, std::function<Clock::time_point()> clock,
      WorkbenchColumnsCallbacks callbacks
  )
      : m_center(center), m_jump(jump), m_clock(std::move(clock)), m_callbacks(std::move(callbacks)) {
    for (std::size_t slot = 0; slot < m_views.size(); ++slot) {
      auto view = std::make_unique<QuietColumnView>(icons, slot, modifiers);
      m_views[slot] = view.get();
      (void)m_center.setColumn(slot, std::move(view));
      wire(slot);
    }
  }

  WorkbenchColumns::~WorkbenchColumns() {
    // Mounted views outlive this object inside the center; they must not call back into it.
    for (auto* view : m_views) {
      view->share = {};
      view->onPointerMoved = {};
      view->onPointerLeft = {};
      view->onRowClick = {};
      view->onRowActivate = {};
      view->onPinControl = {};
      view->onContextRequest = {};
    }
  }

  void WorkbenchColumns::wire(std::size_t slot) {
    auto& view = *m_views[slot];
    const auto frame = [this] {
      if (m_callbacks.frameNeeded) {
        m_callbacks.frameNeeded();
      }
    };
    view.share = [this, slot](EntryId id) -> std::optional<double> {
      if (m_lookahead == nullptr || slot >= m_lookahead->count()) {
        return std::nullopt;
      }
      const auto* entry = m_lookahead->model(slot).entry(id);
      const auto& shares = m_columns[slot].shares;
      const auto found = entry != nullptr ? shares.find(entry->name) : shares.end();
      return found != shares.end() ? std::optional(found->second) : std::nullopt;
    };
    view.onPointerMoved = [this, slot, frame](std::optional<EntryId> row, float sceneX, float sceneY) {
      if (m_state == nullptr) {
        return;
      }
      const auto entry = row ? entryAt(slot, *row) : std::nullopt;
      m_state->pointerInColumn(slot, entry, sceneX, sceneY, m_clock());
      if (entry) {
        m_state->hover(paneOf(slot), *entry);
      } else {
        m_state->unhover(paneOf(slot));
      }
      frame();
    };
    view.onPointerLeft = [this, slot, frame] {
      if (m_state != nullptr) {
        m_state->pointerLeft(m_clock());
        m_state->unhover(paneOf(slot));
        frame();
      }
    };
    view.onRowClick = [this, slot, frame](const QuietRowEvent& event) {
      const auto entry = entryAt(slot, event.entry);
      if (!entry) {
        return;
      }
      if (event.button == BTN_MIDDLE) { // a folder opens in a new tab; a file, as in the list, does nothing
        if (entry->folder) {
          open(*entry, event);
        }
        return;
      }
      if (m_activation == ActivationMode::SingleClick) {
        open(*entry, event);
        return;
      }
      if (m_state != nullptr) {
        m_state->pin(slot, *entry);
        frame();
      }
    };
    view.onRowActivate = [this, slot](const QuietRowEvent& event) {
      if (const auto entry = entryAt(slot, event.entry)) {
        open(*entry, event);
      }
    };
    view.onPinControl = [this, slot, frame] {
      if (m_state == nullptr) {
        return;
      }
      if (m_state->locked() && m_state->pinned(slot)) {
        m_state->unpin();
      } else if (const auto next = nextOf(slot)) {
        m_state->pin(slot, *next);
      }
      frame();
    };
    view.onContextRequest = [this, slot](EntryId id, float sceneX, float sceneY) {
      if (const auto entry = entryAt(slot, id); entry && m_callbacks.context) {
        m_callbacks.context(paneOf(slot), entry->location, sceneX, sceneY);
      }
    };
  }

  void WorkbenchColumns::setSources(LookaheadColumns* lookahead, WorkbenchState* state) {
    m_lookahead = lookahead;
    m_state = state;
    m_fresh = true;
    clear();
  }

  void WorkbenchColumns::setTrail(const FolderTrail* trail) {
    m_trail = trail;
    m_fresh = true;
  }

  std::unique_ptr<Node> WorkbenchColumns::setPreview(std::unique_ptr<Node> preview) {
    takeOut();
    auto previous = std::exchange(m_preview, std::move(preview));
    m_previewNode = m_preview.get();
    return previous;
  }

  void WorkbenchColumns::setListTextSize(float size) {
    for (auto* view : m_views) {
      view->setListTextSize(size);
    }
  }

  QuietColumnView& WorkbenchColumns::view(std::size_t slot) const {
    return *m_views[std::min(slot, m_views.size() - 1)];
  }

  void WorkbenchColumns::update(const PlacedSnapshot& snapshot, const wb::LayoutSettings& settings, bool changed) {
    if (m_lookahead == nullptr || m_state == nullptr) {
      placePreview();
      return;
    }
    m_ready = snapshot.snapshot != nullptr && !snapshot.snapshot->error;
    const bool fresh = std::exchange(m_fresh, false);
    const auto trailed = m_trail != nullptr ? m_trail->serial() : 0;
    if (fresh || changed || m_state->serial() != m_seenState || trailed != m_seenTrail) {
      follow(snapshot, settings);
    }
    m_seenTrail = trailed;
    const auto looked = m_lookahead->serial();
    const bool rows = fresh || changed || looked != m_seenLookahead;
    if (rows) {
      ++m_rowsSerial;
      index(snapshot, settings);
    }
    const auto stateSerial = m_state->serial();
    if (rows || stateSerial != m_seenState) {
      push();
    }
    m_seenState = stateSerial;
    m_seenLookahead = looked;
    if (rows) {
      m_lookahead->collectRetired(); // every column let go of listings no longer shown
    }
    placePreview();
  }

  void WorkbenchColumns::follow(const PlacedSnapshot& snapshot, const wb::LayoutSettings& settings) {
    LookaheadRequest request;
    request.target = m_state->target();
    for (std::size_t column = 0; column < request.pins.size(); ++column) {
      request.pins[column] = m_state->pinned(column);
    }
    if (snapshot.snapshot) {
      wb::ChainFocus focus;
      if (request.target) {
        const auto node = nodeOf(snapshot, request.target->location);
        (m_state->targetSource() == WorkbenchTargetSource::Pointer ? focus.pointer : focus.cursor) = node;
      }
      const auto steer = [&](const std::optional<WorkbenchEntry>& pin) {
        return pin ? nodeOf(snapshot, pin->location) : wb::kNoNode;
      };
      focus.steerB = steer(request.pins[0]);
      focus.steerC = steer(request.pins[1]);
      std::vector<wb::NodeId> remembered;
      if (m_trail != nullptr) {
        for (const auto& child : m_trail->trail(snapshot.snapshot->location, wb::kColumnsAhead)) {
          remembered.push_back(nodeOf(snapshot, child));
        }
      }
      const auto chain = wb::columnChain(snapshot.snapshot->tree, settings, focus, remembered);
      if (!request.target) {
        request.target = entryOf(snapshot, chain.a);
      }
      request.picks = {entryOf(snapshot, chain.b), entryOf(snapshot, chain.c)};
    }
    m_lookahead->follow(request);
  }

  void WorkbenchColumns::index(const PlacedSnapshot& snapshot, const wb::LayoutSettings& settings) {
    for (std::size_t slot = 0; slot < m_columns.size(); ++slot) {
      auto& column = m_columns[slot];
      column.ids.clear();
      column.shares.clear();
      if (slot >= m_lookahead->count()) {
        continue;
      }
      const auto& model = m_lookahead->model(slot);
      for (const auto id : model.rows()) {
        if (const auto* entry = model.entry(id)) {
          column.ids.emplace(entry->name, id);
        }
      }
      if (!snapshot.snapshot) {
        continue;
      }
      const auto& tree = snapshot.snapshot->tree;
      const auto folder = nodeOf(snapshot, m_lookahead->location(slot));
      const auto children = wb::children(tree, folder);
      const auto shares = wb::shares(tree, folder, settings);
      for (std::size_t i = 0; i < children.size() && i < shares.size(); ++i) {
        column.shares.emplace(tree.nodes[children[i]].name, shares[i]);
      }
    }
  }

  std::optional<WorkbenchEntry> WorkbenchColumns::nextOf(std::size_t slot) const {
    if (m_lookahead == nullptr) {
      return std::nullopt;
    }
    if (slot + 1 < m_lookahead->count()) {
      return WorkbenchEntry{.location = m_lookahead->location(slot + 1), .folder = true};
    }
    if (const auto& preview = m_lookahead->preview(); preview && preview->slot == slot + 1) {
      return WorkbenchEntry{.location = preview->file, .folder = false};
    }
    return std::nullopt;
  }

  void WorkbenchColumns::push() {
    ++m_serial;
    const auto& hovered = m_state->hovered();
    for (std::size_t slot = 0; slot < m_views.size(); ++slot) {
      QuietColumnContent content;
      if (slot < m_lookahead->count()) {
        content.listing = &m_lookahead->model(slot);
        content.folder = m_lookahead->location(slot);
        content.rowsSerial = m_rowsSerial;
        content.followed = idOf(slot, nextOf(slot));
        content.pinned = idOf(slot, m_state->pinned(slot));
        if (hovered && hovered->pane != paneOf(slot)) {
          content.linked = idOf(slot, hovered->entry);
        }
        content.columnPinned = m_state->locked() && m_state->pinned(slot).has_value();
      }
      content.noFolders = slot == 0 && m_ready && m_lookahead->count() == 0 && !m_lookahead->preview();
      m_views[slot]->setContent(std::move(content), m_serial);
    }
  }

  void WorkbenchColumns::clear() {
    ++m_serial;
    for (auto* view : m_views) {
      view->setContent({}, m_serial);
    }
    for (auto& column : m_columns) {
      column.ids.clear();
      column.shares.clear();
    }
  }

  bool WorkbenchColumns::placePreview() {
    std::optional<LookaheadPreview> preview = m_lookahead != nullptr ? m_lookahead->preview() : std::nullopt;
    if (preview != m_reported) {
      m_reported = preview;
      if (m_callbacks.preview) {
        m_callbacks.preview(preview); // the owner may hand in its preview node now
      }
    }
    auto home = wb::PreviewHome::None;
    if (preview && m_previewNode != nullptr && preview->slot < m_views.size()) {
      home = wb::previewHome(preview->slot, m_center.plan().columns.size(), m_jumpOpen, m_place);
    }
    const auto slot = home == wb::PreviewHome::Column ? std::optional(preview->slot) : std::nullopt;
    if (home == m_home && slot == m_previewSlot) {
      return false;
    }
    takeOut();
    m_home = home;
    if (home == wb::PreviewHome::Column) {
      m_previewSlot = slot;
      m_parked = m_center.setColumn(*slot, std::move(m_preview));
      m_center.setSpanFrom(m_previewSlot);
    } else if (home == wb::PreviewHome::JumpAhead) {
      (void)m_jump.setPreview(std::move(m_preview));
      m_jump.showPreview(true);
      if (m_state != nullptr) {
        m_state->unhover(WorkbenchPane::JumpAhead); // the hidden list gets no leave event
      }
    }
    return true;
  }

  void WorkbenchColumns::takeOut() {
    if (m_home == wb::PreviewHome::Column && m_previewSlot) {
      m_preview = m_center.setColumn(*m_previewSlot, std::move(m_parked));
      m_previewSlot.reset();
      m_center.setSpanFrom(std::nullopt);
    } else if (m_home == wb::PreviewHome::JumpAhead) {
      m_preview = m_jump.takePreview();
    }
    m_home = wb::PreviewHome::None;
  }

  void WorkbenchColumns::setPreviewPlace(WorkbenchPreviewPlace choice, bool jumpOpen) {
    m_place = choice;
    m_jumpOpen = jumpOpen;
  }

  std::optional<EntryId> WorkbenchColumns::idOf(std::size_t slot, const std::optional<WorkbenchEntry>& entry) const {
    if (!entry || m_lookahead == nullptr || slot >= m_lookahead->count()) {
      return std::nullopt;
    }
    if (entry->location.parent() != m_lookahead->location(slot)) {
      return std::nullopt;
    }
    const auto name = entry->location.basename();
    const auto& ids = m_columns[slot].ids;
    const auto found = name ? ids.find(*name) : ids.end();
    return found != ids.end() ? std::optional(found->second) : std::nullopt;
  }

  std::optional<WorkbenchEntry> WorkbenchColumns::entryAt(std::size_t slot, EntryId id) const {
    if (m_lookahead == nullptr || slot >= m_lookahead->count()) {
      return std::nullopt;
    }
    const auto& model = m_lookahead->model(slot);
    const auto* entry = model.entry(id);
    auto location = model.location(id);
    if (entry == nullptr || !location) {
      return std::nullopt;
    }
    return WorkbenchEntry{.location = std::move(*location), .folder = entry->type == FileType::Directory};
  }

  void WorkbenchColumns::open(const WorkbenchEntry& entry, const QuietRowEvent& event) const {
    const auto& callback = entry.folder ? m_callbacks.go : m_callbacks.activate;
    if (callback) {
      callback({.location = entry.location, .button = event.button, .modifiers = event.modifiers});
    }
  }
} // namespace luciole
