#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "app/page/workbench_controller.h"
#include "app/page/workbench_session.h"
#include "browser/browser_controller.h"
#include "ui/controls/context_menu.h"
#include "ui/workbench/snapshot_places.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {
  using luciole::Location;

  constexpr std::int32_t kMenuOpen = 1;
  constexpr std::int32_t kMenuOpenInNewTab = 2;
  constexpr float kMoreMenuWidth = 340;
} // namespace

void WorkbenchController::contextMenu(const luciole::WorkbenchContextRequest& request) {
  if (!request.entry) {
    m_menus.openFileMenu(std::nullopt, request.sceneX, request.sceneY);
    return;
  }
  auto& model = m_page.browser().model();
  if (const auto id = luciole::rowOf(model, *request.entry)) {
    // As the list does on a secondary press: the row joins the selection the menu acts on.
    if (!model.selection().selected.contains(*id)) {
      (void)model.select(*id);
    } else {
      (void)model.setFocus(id);
    }
    m_page.requestSync();
    m_menus.openFileMenu(id, request.sceneX, request.sceneY);
    return;
  }
  const auto isFolder = folder(*request.entry);
  if (!isFolder) {
    return;
  }
  std::vector<ContextMenuControlEntry> entries{{.id = kMenuOpen, .label = "Open"}};
  if (*isFolder) {
    entries.push_back({.id = kMenuOpenInNewTab, .label = "Open in New Tab"});
  }
  m_menus.showContextMenu(
      std::move(entries),
      [this, location = *request.entry, isFolder = *isFolder](int id) {
        if (id == kMenuOpenInNewTab) {
          openInNewTab(location, 0);
        } else if (isFolder) {
          m_page.browser().open(location);
        } else {
          openFile(location);
        }
      },
      {request.sceneX, request.sceneY, 1, 1}
  );
}

void WorkbenchController::showMore(const luciole::WorkbenchMoreRequest& request) {
  m_menus.showContextMenu(
      luciole::moreMenuEntries(request),
      [this, listed = request.listed](int id) {
        if (const auto choice = luciole::moreChoice(listed, id)) {
          if (choice->action == luciole::MoreAction::Go) {
            go(choice->request);
          } else {
            select(choice->request);
          }
        }
      },
      {request.sceneX, request.sceneY, 1, 1}, false, kMoreMenuWidth
  );
}

std::optional<bool> WorkbenchController::folder(const Location& location) const {
  const auto parent = location.parent();
  const auto& lookahead = m_session->lookahead();
  for (std::size_t column = 0; parent && column < lookahead.count(); ++column) {
    if (lookahead.location(column) == *parent) {
      const auto& model = lookahead.model(column);
      if (const auto id = luciole::rowOf(model, location)) {
        return model.entry(*id)->type == luciole::FileType::Directory;
      }
    }
  }
  if (const auto snapshot = m_session->model().snapshot()) {
    const auto node = luciole::nodeOf(luciole::placeSnapshot(snapshot), location);
    if (node < snapshot->tree.nodes.size()) {
      return snapshot->tree.nodes[node].folder;
    }
  }
  return std::nullopt;
}
