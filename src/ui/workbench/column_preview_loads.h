#pragma once

#include "browser/workbench_services.h"
#include "fs/directory_entries.h"
#include "ui/controls/audio_visual_view.h"
#include "ui/workbench/column_preview_content.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace luciole {
  class ColumnPreviewView;

  struct ColumnPreviewRequest {
    DirectoryEntry entry;
    ColumnPreviewKind kind = ColumnPreviewKind::Still;
    float textSize = 12;    // the header's; the content's is this times `scale`
    float scale = 1;        // the preview size as a factor
    int posterPixels = 512; // the slot's longest side, in buffer pixels
    // A sound file's visualizer: the choice it starts with, the user's choice made with its buttons, and the
    // keyboard modifiers held.
    AudioVisualChoice audioVisual = {};
    std::function<void(const AudioVisualChoice&)> audioVisualChosen = nullptr;
    // A video or sound file's loop switch: the choice it starts with and the user's choice made with its button.
    bool mediaLoop = false;
    std::function<void(bool)> mediaLoopChosen = nullptr;
    std::function<std::uint32_t()> modifiers = nullptr;
  };

  enum class ColumnPreviewStatus : std::uint8_t {
    Loading, // nothing to show yet
    Shown,   // the content, or the words of a refusal such as a private key
    Failed,  // a sentence saying why nothing is shown
    Still,   // the content check sends the file back to the page's own preview
  };

  // One file's content in a column preview. It puts its view into the column preview at once, loads off the main
  // context and keeps the view current. Destroying it cancels its work, stops its view's timers, workers and
  // playback, and frees the content. Main context only.
  class ColumnPreviewLoad {
  public:
    virtual ~ColumnPreviewLoad();
    ColumnPreviewLoad(const ColumnPreviewLoad&) = delete;
    ColumnPreviewLoad& operator=(const ColumnPreviewLoad&) = delete;
    ColumnPreviewLoad(ColumnPreviewLoad&&) = delete;
    ColumnPreviewLoad& operator=(ColumnPreviewLoad&&) = delete;

    // Once per frame, before layout.
    virtual void update() {}
    // The header's text size and the preview size as a factor: text is drawn at their product, pictures and models
    // that many times as large; video and sound stay fitted, their controls at the header's size.
    virtual void setSize(float textSize, float scale) = 0;
    // A sound file's visualizer choice made elsewhere, applied at once; other kinds have none.
    virtual void setAudioVisual(const AudioVisualChoice& /*choice*/) {}
    // The loop switch changed elsewhere, applied at once; other kinds have none.
    virtual void setMediaLoop(bool /*loop*/) {}

    ColumnPreviewStatus status() const noexcept { return m_status; }
    // What the content knows of the file: lines, rows and columns, entries, playing time or triangles.
    const ColumnPreviewFacts& facts() const noexcept { return m_facts; }
    // The file is shown as text, whole or its first part, or refused as a private key: the kinds an edit bar offers.
    bool showsText() const noexcept { return m_showsText; }
    // The file is shown as a table of its own: CSV, TSV or a workbook's sheet. An edit bar is offered for these too.
    bool showsTable() const noexcept { return m_showsTable; }

  protected:
    ColumnPreviewLoad(ColumnPreviewView& view, std::function<void()> frameNeeded);

    // Fails with `message` shown in place of the content.
    void fail(std::string message);
    void needFrame() const;
    // Expires when the load goes: callbacks from workers check it before touching the load.
    std::weak_ptr<int> lifetime() const { return m_lifetime; }

    ColumnPreviewView& m_view;
    ColumnPreviewStatus m_status = ColumnPreviewStatus::Loading;
    ColumnPreviewFacts m_facts;
    bool m_showsText = false;
    bool m_showsTable = false;

  private:
    std::function<void()> m_frameNeeded;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };

  // Starts showing `request` in `view`; null for Still, which the page's own preview shows. Text and Binary need
  // the text service of `workers` and throw std::invalid_argument without it; a still picture and a media file's
  // poster come from its thumbnail service, and a picture without one goes back to the page's own preview.
  std::unique_ptr<ColumnPreviewLoad> startColumnPreviewLoad(
      ColumnPreviewView& view, const ColumnPreviewRequest& request, const WorkbenchPreviewWorkers& workers,
      std::function<void()> frameNeeded
  );
} // namespace luciole
