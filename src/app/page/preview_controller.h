#pragma once

#include "fs/directory_entries.h"
#include "render/scene/node.h"

#include <memory>
#include <optional>
#include <utility>

namespace luciole {
  class ColumnPreviewPage;
  class FileIconCache;
  class PreviewService;
  class ThumbnailService;
} // namespace luciole

struct KeyboardEvent;
class ApplicationChooserController;
class FilePreviewView;
class PageContext;
class RenderContext;

// An entry previewed in a rectangle of the page instead of the selection, below a Workbench column slot's header,
// with all its text at `textSize`, which the owner keeps below the list's.
struct PreviewTarget {
  luciole::DirectoryEntry entry;
  LayoutRect bounds;
  float textSize = 0;
};

// File preview: the preview pane beside the file list, the quick preview dialog, and the slot's view of a target.
class PreviewController {
public:
  PreviewController(
      PageContext& page, luciole::PreviewService& preview, luciole::ThumbnailService& thumbnails,
      luciole::FileIconCache& icons, RenderContext& renderer, ApplicationChooserController& applications
  );

  // Asks the preview service for the selected item at the size the pane or popup shows it.
  void sync();
  void showQuickPreview();
  // Keyboard hook in BrowserPage::keyboard() while the popup is shown: the quick preview closes and steps through
  // items.
  bool quickPreviewKey(const KeyboardEvent& event);
  // Layout hook in BrowserPage::prepare(): a shown target, else the pane when `show`; hides the rest.
  void layoutPane(bool show);
  // BrowserPage::deactivate(): hides the pane and the target's view and clears their images.
  void hidePane();
  // Dialog frame hook, before the dialog measures itself: sizes and syncs the quick preview.
  void layoutQuickPreview();

  // Pane rectangle the workspace reserves; empty while no pane fits.
  const std::optional<LayoutRect>& bounds() const { return m_bounds; }
  void setBounds(std::optional<LayoutRect> bounds) { m_bounds = bounds; }
  // Shows `target` in its rectangle, in place of the pane, while the page is visible and the quick preview is
  // closed; none returns to the selection. Throws std::invalid_argument for a text size that is not positive and
  // finite.
  void setTarget(std::optional<PreviewTarget> target);

private:
  // The target is shown now.
  bool targetShown() const;
  // Shows the target in its view, made on first use, or hides that view.
  void layoutTarget(bool shown);
  void openPreviewed(bool with);
  std::unique_ptr<FilePreviewView> makeView(bool quick);
  // The quick preview's content size: the Medium dialog width and a height that follows the window.
  std::pair<float, float> quickPreviewSize() const;

  PageContext& m_page;
  luciole::PreviewService& m_preview;
  luciole::ThumbnailService& m_thumbnails;
  luciole::FileIconCache& m_icons;
  RenderContext& m_renderer;
  ApplicationChooserController& m_applications;
  FilePreviewView* m_view = nullptr;
  FilePreviewView* m_quickPreview = nullptr;
  luciole::ColumnPreviewPage* m_targetView = nullptr;
  std::optional<LayoutRect> m_bounds;
  std::optional<PreviewTarget> m_target;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
