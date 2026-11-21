#pragma once

#include "app/page/preview_edit_page.h"
#include "app/page/workbench_session.h"
#include "browser/action_registry.h"
#include "browser/directory_model.h"
#include "config/settings.h"
#include "core/timer_manager.h"
#include "fs/location.h"
#include "render/scene/node.h"
#include "ui/signal.h"
#include "ui/workbench/workbench_columns.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace luciole {
  class FileIconCache;
  class FileViewAdapter;
  class ViewPreferenceWriter;
  class VisitService;
  class WorkbenchPreview;
  class WorkbenchServices;
  struct WorkbenchContextRequest;
  struct WorkbenchEntry;
  struct WorkbenchEntryRequest;
  struct WorkbenchGeometry;
  struct WorkbenchMoreRequest;
} // namespace luciole

struct KeyboardEvent;
struct PointerEvent;
class MenuController;
class PageContext;
class PreviewController;

// The Workbench mode of a page. While the mode is on it keeps a WorkbenchSession, which holds the page's file pane
// in the view's center; the page's browser stays the one every action, the status bar and drags act on. Each frame it
// feeds the session from that browser, and it carries out what the view asks: navigation through the page's browser
// so history works as everywhere else, selection, launching, menus, saving the choices and the geometry, and the
// file a column previews.
class WorkbenchController {
public:
  // What only the page can do: hand a file to its program, show the archive browser, read the keyboard modifiers, and
  // tell whether one of its text fields has focus. All are required.
  struct Hooks {
    std::function<void(const luciole::Location&)> launch;
    std::function<void(const luciole::ArchiveFacts&)> browseArchive;
    std::function<std::uint32_t()> modifiers;
    std::function<bool()> editing;
  };

  WorkbenchController(
      PageContext& page, MenuController& menus, PreviewController& preview, luciole::FileViewAdapter& list,
      luciole::FileIconCache& icons, luciole::VisitService& visits, luciole::ViewPreferenceWriter& preferences,
      Hooks hooks
  );
  ~WorkbenchController();
  WorkbenchController(const WorkbenchController&) = delete;
  WorkbenchController& operator=(const WorkbenchController&) = delete;
  WorkbenchController(WorkbenchController&&) = delete;
  WorkbenchController& operator=(WorkbenchController&&) = delete;

  void load(const luciole::BrowserSettings& settings);
  // The application's shared measures; the mode shows the view once they are set.
  void setServices(luciole::WorkbenchServices& services);
  // Receives each folder the page's browser enters and each file it opens; unset, nothing is recorded.
  void setVisitRecorder(std::function<void(const luciole::Location&)> record) { m_recordVisit = std::move(record); }
  void recordVisit(const luciole::Location& location) const {
    if (m_recordVisit) {
      m_recordVisit(location);
    }
  }

  // Frame slot in BrowserPage::prepareFrame(): mounts or unmounts the view with the mode and feeds it.
  void onFrame();
  // BrowserPage::prepare(), after layout and before the preview syncs: the column slot that previews a file, and
  // the page's preview laid over it for the kinds the column preview leaves to that preview.
  void placePreview();
  // BrowserPage::hide(): the page gets no more frames, so the column preview lets its content go now; the next
  // placePreview() loads it again.
  void hide();
  void applySettings(const luciole::BrowserSettings& before, const luciole::BrowserSettings& settings, bool force);
  // The visit service changed.
  void visitsChanged();
  // Pointer hook in BrowserPage::pointer(): the list rows the pointer moves over and rests on.
  void pointer(const PointerEvent& event, float x, float y);
  // Axis hook in BrowserPage::dispatchPointer(), before the page's dispatcher: Ctrl with the wheel over the column
  // preview's content steps the preview size. True when taken.
  bool previewWheel(const PointerEvent& event, float x, float y);
  // The pointer went to another page or the tab strip, which ends the list's hover.
  void pointerLeft();
  // Keyboard hook in BrowserPage::keyboard(), before the page's shortcuts: the keys of the view's focused part, and
  // Escape to minimize an expanded instrument while no text field of the page has focus.
  bool key(const KeyboardEvent& event);

  // The Workbench shows or reads something inside the drive at `root`: a listing ahead, the previewed file, or the
  // page's walks. The page then leaves the drive, as it does when its own folder is inside it.
  bool inDrive(const luciole::Location& root) const;
  // Opens a file of the view as Open does in the list: an archive shows the archive browser, else its program.
  void openFile(const luciole::Location& location);
  // The editor holds unsaved changes to a file of the drive at `root`.
  bool unsavedIn(const luciole::Location& root);
  // The drive at `root` is being removed: its walks, listings ahead and previewed file go.
  void releaseLocation(const luciole::Location& root);
  // Something of the Workbench still reads the drive at `root`: a walk of the shared service, which may outlive its
  // cancelled lease, a listing ahead or the previewed file.
  bool readingDrive(const luciole::Location& root) const;
  // The view hides the file pane under the expanded instrument. The pane keeps its geometry meanwhile, so the page's
  // own hit tests on it must check this first.
  bool coversFilePane() const;
  // The file pane's place in `frame`'s coordinates while the view shows it, with the room below the list's rows
  // where the status pill floats; none otherwise. A change of folder slides the pane, not its place.
  std::optional<LayoutRect> listArea(const Node& frame) const {
    return m_session ? m_session->paneArea(frame) : std::nullopt;
  }
  // A divider of the view is dragged; BrowserPage::escapeGesture() cancels it.
  bool resizing() const;
  // The mode shows no docked preview, so the page reports none and the preview's shortcut changes nothing.
  bool hidesDockedPreview() const;
  // ~BrowserPage(), before the scene goes: gives the file pane back and frees the view.
  void close();
  // The column preview's editor, made on first use. It lives as long as the page, so a question about unsaved changes
  // still opens, and Save still writes, after another view mode took the view away.
  luciole::PreviewEditController& editor() {
    if (!m_editor) {
      m_editor = pagePreviewEditor(m_page, m_hooks.modifiers);
    }
    return *m_editor;
  }

private:
  struct ScenePoint {
    float x = 0;
    float y = 0;
  };

  void mount();
  void unmount();
  void wire();
  void applyGeometry();
  void go(const luciole::WorkbenchEntryRequest& request);
  void select(const luciole::WorkbenchEntryRequest& request);
  void contextMenu(const luciole::WorkbenchContextRequest& request);
  // The entries a merged piece stands for, as a menu near it; choosing one does what a click on its piece does.
  void showMore(const luciole::WorkbenchMoreRequest& request);
  // The page's preview over the column slot at `placement` with its text at `textSize`, or none.
  void overlay(const std::optional<luciole::ColumnPreview::Placement>& placement, float textSize);
  void markList(const std::optional<luciole::Location>& location);
  void rebindRow(std::optional<luciole::EntryId> id);
  void saveGeometry(const luciole::WorkbenchGeometry& geometry);
  void openInNewTab(const luciole::Location& location, std::uint32_t modifiers);
  // Whether `location` is a folder, as a column ahead or the measured tree knows it; none when neither does.
  std::optional<bool> folder(const luciole::Location& location) const;
  // The pointer at `at` on the page, or gone from it: a row of the shown list becomes the hover and may start a rest.
  void pointList(std::optional<ScenePoint> at);
  void restAt();
  void frameNeeded();

  PageContext& m_page;
  MenuController& m_menus;
  PreviewController& m_preview;
  luciole::FileViewAdapter& m_list;
  luciole::FileIconCache& m_icons;
  luciole::VisitService& m_visits;
  luciole::ViewPreferenceWriter& m_preferences;
  Hooks m_hooks;
  std::function<void(const luciole::Location&)> m_recordVisit;
  luciole::WorkbenchServices* m_services = nullptr;
  luciole::BrowserSettings m_settings; // with the choices and geometry this page saved since
  luciole::FolderTrail m_trail;        // every folder this page entered while it lives, in any mode
  std::optional<luciole::Location> m_visited;
  std::unique_ptr<luciole::PreviewEditController> m_editor; // goes after the preview it is attached to
  std::unique_ptr<luciole::WorkbenchSession> m_session;
  std::unique_ptr<luciole::WorkbenchPreview> m_columnPreview; // what the column slot shows; goes before the session
  Signal<>::ScopedConnection m_paletteConn;
  Timer m_restTimer;
  std::optional<std::chrono::steady_clock::time_point> m_restDeadline;
  bool m_pointerInList = false;
};
