#include "app/drive_leaving.h"
#include "app/workspace_application.h"
#include "core/deferred_call.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

bool WorkspaceApplication::unsaved(luciole::TabId page) const {
  const auto found = m_pages.find(page);
  return found != m_pages.end() && found->second->unsaved();
}

void WorkspaceApplication::leaveThen(std::vector<luciole::TabId> pages, std::function<void()> action) {
  // Nothing to ask: the action runs now, also while another request waits.
  if (std::ranges::none_of(pages, [this](auto page) { return unsaved(page); })) {
    action();
    return;
  }
  std::vector<luciole::CloseGate::Page> asked;
  asked.reserve(pages.size());
  for (const auto page : pages) {
    asked.push_back(
        {.unsaved = [this, page] { return unsaved(page); },
         .ask =
             [this, page](std::function<void()> resume) {
               reveal(page);
               return m_pages.at(page)->leave(std::move(resume));
             }}
    );
  }
  m_leaving.request(std::move(asked), std::move(action));
}

void WorkspaceApplication::askBeforeDriveLeave(
    const luciole::Location& root, std::function<void()> action, std::function<void()> abandoned
) {
  // The focused window's pages first, then those of the other windows.
  auto windows = std::vector<luciole::WindowId>{};
  for (const auto& [id, record] : m_model.windows()) {
    (void)record;
    windows.push_back(id);
  }
  std::ranges::stable_partition(windows, [this](auto id) { return id == m_focused; });
  std::vector<luciole::DriveEditor> editors;
  for (const auto window : windows) {
    for (const auto page : windowPages(window)) {
      editors.push_back(
          {.unsavedIn =
               [this, page](const luciole::Location& drive) {
                 const auto found = m_pages.find(page);
                 return found != m_pages.end() && found->second->workbench().unsavedIn(drive);
               },
           .ask =
               [this, page](std::function<void()> resume) {
                 reveal(page);
                 return m_pages.at(page)->leave(std::move(resume));
               }}
      );
    }
  }
  luciole::leaveDriveThen(m_leaving, root, std::move(editors), std::move(action), std::move(abandoned));
}

void WorkspaceApplication::executeAsked(
    luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command, std::optional<luciole::Location> location
) {
  // Asking shows each page as the active pane of the selected tab; the command acts on the panes as they were, and
  // the tab selected before comes back.
  const auto active = m_model.layout(tab).transform([](const auto& layout) { return layout.active; });
  const auto selected = m_model.windows().at(window).selected;
  auto pages = leavingPages(window, tab, command);
  const bool asks = std::ranges::any_of(pages, [this](auto page) { return unsaved(page); });
  leaveThen(std::move(pages), [this, window, tab, command, location = std::move(location), active, selected, asks] {
    if (m_model.owner(tab) != window) {
      return;
    }
    if (active) {
      (void)m_model.focus(window, *active);
    }
    execute(window, tab, command, location);
    if (asks && m_model.owner(selected) == window) {
      select(window, selected);
    }
  });
}

std::vector<luciole::TabId>
WorkspaceApplication::leavingPages(luciole::WindowId window, luciole::TabId tab, WorkspaceCommand command) const {
  const auto layout = m_model.layout(tab);
  if (!layout) {
    return {};
  }
  switch (command) {
  case WorkspaceCommand::Close:
    return m_model.pages(tab);
  case WorkspaceCommand::ClosePane: {
    // The pane execute() closes: a logical tab stands for its active pane.
    const auto logical = m_model.tabFor(tab);
    const auto page = logical && *logical == tab ? layout->active : tab;
    const auto& record = m_model.windows().at(window);
    if (record.secondary && (page == record.primary || page == *record.secondary)) {
      return {page};
    }
    return {};
  }
  case WorkspaceCommand::Split:
    // Leaving split view keeps the active pane.
    if (layout->secondary) {
      return {layout->active == layout->primary ? *layout->secondary : layout->primary};
    }
    return {};
  default:
    // Detach and MoveWindow carry the pages, with their Workbench session and editor, to the other window.
    return {};
  }
}

std::vector<luciole::TabId> WorkspaceApplication::windowPages(luciole::WindowId window) const {
  const auto found = m_model.windows().find(window);
  if (found == m_model.windows().end()) {
    return {};
  }
  auto tabs = found->second.tabs;
  std::ranges::stable_partition(tabs, [selected = found->second.selected](auto tab) { return tab == selected; });
  std::vector<luciole::TabId> pages;
  for (const auto tab : tabs) {
    const auto panes = m_model.pages(tab);
    pages.insert(pages.end(), panes.begin(), panes.end());
  }
  return pages;
}

void WorkspaceApplication::reveal(luciole::TabId page) {
  const auto window = m_model.owner(page);
  const auto tab = m_model.tabFor(page);
  if (!window || !tab || !m_hosts.contains(*window)) {
    return;
  }
  auto& host = *m_hosts.at(*window);
  const bool elsewhere = *window != m_focused;
  select(*window, *tab);
  (void)m_model.focus(*window, page);
  host.surface.requestLayout();
  prepare(host);
  if (elsewhere) {
    m_connection.activateSurface(host.surface.wlSurface());
  }
}

bool WorkspaceApplication::mayClose(luciole::WindowId window) {
  // A close the gate's action starts was agreed to by every page of the window.
  if (g_stopRequested || m_leaving.acting() || std::ranges::none_of(windowPages(window), [this](auto page) {
        return unsaved(page);
      })) {
    return true;
  }
  // close() is running: the questions open on the next main loop iteration.
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime, window] {
    if (lifetime.expired() || !m_hosts.contains(window)) {
      return;
    }
    leaveThen(windowPages(window), [this, window] {
      if (const auto host = m_hosts.find(window); host != m_hosts.end()) {
        host->second->surface.close();
      }
    });
  });
  return false;
}

void WorkspaceApplication::quit() {
  auto pages = windowPages(m_focused);
  for (const auto& [id, host] : m_hosts) {
    (void)host;
    if (id != m_focused) {
      const auto more = windowPages(id);
      pages.insert(pages.end(), more.begin(), more.end());
    }
  }
  leaveThen(std::move(pages), [] { g_stopRequested = 1; });
}
