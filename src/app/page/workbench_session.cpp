#include "app/page/workbench_session.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/builders.h"
#include "ui/controls/pane_divider.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"
#include "ui/workbench/workbench_center_view.h"
#include "workbench/format.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <linux/input-event-codes.h>
#include <memory>
#include <stdexcept>
#include <utility>

namespace luciole {
  namespace {
    // Where `child` sits among the children of `parent`; past the end when it is not there.
    std::size_t indexOf(const Node& parent, const Node* child) {
      const auto& children = parent.children();
      std::size_t index = 0;
      while (index < children.size() && children[index].get() != child) {
        ++index;
      }
      return index;
    }

    bool sameRect(const LayoutRect& a, const LayoutRect& b) {
      return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    // The bottom padding of the file pane (BrowserPage::prepareFrame()), which lies under the status tab already.
    constexpr float kPaneBottomPadding = Style::spaceSm;

    // Hands a wheel turned in `room` to the areas of `list` that take a wheel over the list, from its focus area up,
    // as the dispatcher walks them.
    bool scrollFromRoom(const Node& room, const VirtualGridView& list, const InputArea::PointerData& data) {
      float x = 0;
      float y = 0;
      Node::mapToScene(&room, data.localX, data.localY, x, y);
      for (Node* at = list.focusArea(); at != nullptr && at != &list; at = at->parent()) { // bounded by the depth
        auto* area = dynamic_cast<InputArea*>(at);
        if (area == nullptr || !area->enabled()) {
          continue;
        }
        float localX = 0;
        float localY = 0;
        Node::mapFromScene(area, x, y, localX, localY);
        if (area->dispatchAxis(
                localX, localY, data.axis, data.axisSource, data.axisValue, data.axisDiscrete, data.axisValue120,
                data.axisLines
            )) {
          return true;
        }
      }
      return false;
    }
  } // namespace

  WorkbenchFeed workbenchFeed(const BrowserController& browser) {
    const auto& model = browser.model();
    WorkbenchFeed feed{
        .folder = model.snapshot().location,
        .cursor = std::nullopt,
        .sort = model.sort(),
        .showHidden = model.filter().showHidden,
        .showBackups = model.filter().showBackups,
    };
    if (const auto focused = model.selection().focused) {
      if (const auto* entry = model.entry(*focused)) {
        feed.cursor = WorkbenchEntry{.location = entry->location(), .folder = entry->type == FileType::Directory};
      }
    }
    return feed;
  }

  std::optional<EntryId> rowOf(const DirectoryModel& model, const Location& location) {
    const auto name = location.basename();
    if (!name) {
      return std::nullopt;
    }
    // Names first: resolving a location costs far more than comparing one.
    for (const auto id : model.rows()) {
      const auto* entry = model.entry(id);
      if (entry != nullptr && entry->name == *name && entry->location() == location) {
        return id;
      }
    }
    return std::nullopt;
  }

  bool restoreCursor(DirectoryModel& model, const Location& file) {
    if (model.selection().focused) {
      return false;
    }
    const auto id = rowOf(model, file);
    if (!id) {
      return false;
    }
    return model.selection().selected.contains(*id) ? model.setFocus(id) : model.select(*id, SelectionMode::Toggle);
  }

  std::optional<WorkbenchEntry>
  listEntryAt(const DirectoryModel& model, const VirtualGridView& list, float x, float y) {
    const auto index = list.itemAtScene(x, y);
    if (!index || *index >= model.rows().size()) {
      return std::nullopt;
    }
    const auto* entry = model.entry(model.rows()[*index]);
    if (entry == nullptr) {
      return std::nullopt;
    }
    return WorkbenchEntry{.location = entry->location(), .folder = entry->type == FileType::Directory};
  }

  std::optional<PreviewedEntry>
  previewedEntry(const LookaheadColumns& lookahead, const DirectoryModel& list, const LookaheadPreview& preview) {
    const DirectoryModel* model = &list;
    if (preview.slot > 0) {
      const auto column = preview.slot - 1;
      if (column >= lookahead.count() || preview.file.parent() != lookahead.location(column)) {
        return std::nullopt;
      }
      model = &lookahead.model(column);
    }
    const auto id = rowOf(*model, preview.file);
    if (!id) {
      return std::nullopt;
    }
    return PreviewedEntry{.entry = *model->entry(*id), .typeLabel = std::string(model->typeLabel(*id))};
  }

  std::vector<ContextMenuControlEntry> moreMenuEntries(const WorkbenchMoreRequest& request) {
    constexpr std::size_t kFolderChars = 32;
    const auto count = [](std::size_t value) { return workbench::countText(static_cast<double>(value)); };
    const auto folder = request.folder.displayName().value_or(request.folder.uri());
    std::vector<ContextMenuControlEntry> rows;
    rows.reserve(request.listed.size() + 2);
    rows.push_back({
        .label = std::format("{} more in {}", count(request.count), workbench::middleEllipsis(folder, kFolderChars)),
        .header = true,
    });
    for (std::size_t i = 0; i < request.listed.size(); ++i) {
      const auto& listed = request.listed[i];
      rows.push_back({
          .id = static_cast<std::int32_t>(i + 1),
          .label = listed.entry.location.displayName().value_or(listed.name),
          .ellipsize = TextEllipsize::Middle,
          .shortcut = listed.value,
      });
    }
    if (request.count > request.listed.size()) {
      rows.push_back(
          {.label = std::format("{} more not listed", count(request.count - request.listed.size())), .enabled = false}
      );
    }
    return rows;
  }

  std::optional<MoreChoice> moreChoice(const std::vector<MergedEntry>& listed, int id) {
    if (id < 1 || static_cast<std::size_t>(id) > listed.size()) {
      return std::nullopt;
    }
    const auto& entry = listed[static_cast<std::size_t>(id) - 1].entry;
    return MoreChoice{
        .action = entry.folder ? MoreAction::Go : MoreAction::Select,
        .request = {.location = entry.location, .button = BTN_LEFT, .modifiers = 0},
    };
  }

  bool takesPreviewKey(ViewMode mode, std::uint32_t sym, std::uint32_t modifiers) {
    return mode == ViewMode::Workbench && shortcutMatches(BrowserAction::ShowPreview, sym, modifiers);
  }

  LayoutRect previewBounds(const LayoutRect& slot) {
    constexpr float kDividerHalf = PaneDivider::kHitSize / 2;
    const float width = std::isfinite(slot.width) ? std::max(slot.width, 0.0F) : 0.0F;
    const float inset = std::min(kDividerHalf, width);
    return {
        .x = Style::rtl() ? slot.x : slot.x + inset,
        .y = slot.y,
        .width = width - inset,
        .height = slot.height,
    };
  }

  LayoutRect jumpPreviewBounds(const LayoutRect& slot) {
    constexpr float kDividerHalf = PaneDivider::kHitSize / 2;
    const float width = std::isfinite(slot.width) ? std::max(slot.width, 0.0F) : 0.0F;
    const float inset = std::min(kDividerHalf, width);
    return {
        .x = Style::rtl() ? slot.x + inset : slot.x,
        .y = slot.y,
        .width = width - inset,
        .height = slot.height,
    };
  }

  bool
  ColumnPreview::update(const LookaheadColumns& lookahead, const DirectoryModel& list, std::optional<LayoutRect> slot) {
    const auto& preview = lookahead.preview();
    if (!preview || !slot) {
      const bool placed = m_placement.has_value();
      m_placement.reset();
      return placed;
    }
    const auto generation = list.snapshot().generation;
    if (m_placement && lookahead.serial() == m_serial && generation == m_generation) {
      if (sameRect(m_placement->bounds, *slot)) {
        return false;
      }
      m_placement->bounds = *slot;
      return true;
    }
    m_serial = lookahead.serial();
    m_generation = generation;
    auto found = previewedEntry(lookahead, list, *preview);
    if (!found) {
      const bool placed = m_placement.has_value();
      m_placement.reset();
      return placed;
    }
    m_placement = Placement{.previewed = std::move(*found), .bounds = *slot};
    return true;
  }

  WorkbenchSession::WorkbenchSession(
      Node& pane, InputArea* listFocus, MeasureCache& cache, FileIconCache& icons,
      const std::function<std::uint32_t()>& modifiers, WorkbenchModelOptions options
  )
      : m_model(cache, options), m_lookahead({}, false, true), m_parent(pane.parent()), m_pane(&pane) {
    if (m_parent == nullptr) {
      throw std::invalid_argument("the file pane has no parent to take its place in");
    }
    const auto index = indexOf(*m_parent, &pane);
    auto list = m_parent->removeChild(&pane);
    auto view = std::make_unique<WorkbenchView>(icons, modifiers);
    view->setFlexGrow(list->flexGrow());
    view->setModel(&m_model);
    view->setLookahead(&m_lookahead);
    view->setState(&m_state);
    (void)view->setList(std::move(list), listFocus);
    auto slot = std::make_unique<ColumnPreviewView>();
    m_slot = slot.get();
    (void)view->setPreview(std::move(slot));
    m_view = static_cast<WorkbenchView*>(m_parent->insertChildAt(index, std::move(view)));
  }

  WorkbenchSession::~WorkbenchSession() {
    m_model.setChangedCallback({});
    m_lookahead.setChangedCallback({});
    m_view->setModel(nullptr);
    m_view->setLookahead(nullptr);
    m_view->setState(nullptr);
    auto pane = m_view->setList(nullptr, nullptr);
    if (m_room != nullptr) {
      pane->removeChild(m_room).reset();
    }
    const auto index = indexOf(*m_parent, m_view);
    m_parent->removeChild(m_view).reset();
    m_parent->insertChildAt(index, std::move(pane));
  }

  void WorkbenchSession::keepTabRoom(VirtualGridView& list) {
    if (m_room != nullptr) {
      return;
    }
    m_room = m_pane->addChild(
        ui::inputArea({
            .height = 0.0F,
            .onAxisHandler = [this, &list](const InputArea::PointerData& data) {
              return scrollFromRoom(*m_room, list, data);
            },
        })
    );
  }

  void WorkbenchSession::setTabRoom(float tabHeight) {
    const float height = std::max(0.0F, tabHeight - kPaneBottomPadding);
    if (m_room != nullptr && m_room->height() != height) {
      m_room->setSize(m_room->width(), height);
      m_room->markLayoutDirty();
    }
  }

  std::optional<LayoutRect> WorkbenchSession::paneArea(const Node& frame) const {
    if (!showsPane()) {
      return std::nullopt;
    }
    // the parent's position leaves out the pane's own slide
    float x = 0;
    float y = 0;
    Node::absolutePosition(m_pane->parent(), x, y);
    float frameX = 0;
    float frameY = 0;
    Node::absolutePosition(&frame, frameX, frameY);
    return LayoutRect{
        .x = x + m_pane->x() - frameX,
        .y = y + m_pane->y() - frameY,
        .width = m_pane->width(),
        .height = m_pane->height(),
    };
  }

  bool WorkbenchSession::shows(const Node& node) const {
    // Bounded by the depth of the scene.
    for (const Node* at = &node; at != nullptr; at = at->parent()) {
      if (!at->visible()) {
        return false;
      }
      if (at == m_view) {
        return true;
      }
    }
    return false;
  }

  void WorkbenchSession::update(const WorkbenchFeed& feed, const WorkbenchChoices& choices) {
    if (feed.folder && (!m_feed || feed.folder != m_feed->folder)) {
      m_model.setLocation(*feed.folder);
      m_view->setFolder(*feed.folder);
    }
    const bool listing = !m_feed
        || feed.sort != m_feed->sort
        || feed.showHidden != m_feed->showHidden
        || feed.showBackups != m_feed->showBackups;
    if (listing) {
      m_lookahead.setListing(feed.sort, feed.showHidden, feed.showBackups);
    }
    if (choices.layout != m_layout) {
      m_layout = choices.layout;
      m_model.setSettings(choices.layout);
    }
    m_view->setChoices(choices);
    m_view->setCursor(feed.cursor);
    m_feed = feed;
    m_view->update();
  }
} // namespace luciole
