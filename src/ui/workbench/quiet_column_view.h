#pragma once

#include "browser/directory_model.h"
#include "fs/location.h"
#include "thumbnails/file_icon_cache.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class Box;
class Button;
class Label;
class VirtualGridView;

namespace luciole {
  class GridPointer;

  enum class QuietNotice : std::uint8_t { None, Loading, Empty, Unreadable, Gone, NoFolders };

  // What a column ahead shows.
  struct QuietColumnContent {
    // Borrowed until the next setContent; none shows no rows and no notice.
    const DirectoryModel* listing = nullptr;
    std::optional<Location> folder = std::nullopt; // the listed folder, named in the header
    // Changes whenever the listing's rows or entries, or the shares, change: every shown row binds again.
    std::uint64_t rowsSerial = 0;
    std::optional<EntryId> followed = std::nullopt; // the entry the next column lists or previews
    std::optional<EntryId> pinned = std::nullopt;
    std::optional<EntryId> linked = std::nullopt; // the entry another pane points at
    bool columnPinned = false;                    // the header's pin control is on
    bool noFolders = false; // no listing: the column says there are no folders ahead, instead of drawing nothing
  };

  struct QuietRowEvent {
    EntryId entry = 0;
    std::uint32_t button = 0;
    std::uint32_t modifiers = 0;
  };

  // One column ahead of a Workbench list: a header with the folder's name, its entry count and a pin control,
  // over a virtualized list of the listing's rows in its order, or a notice while the folder loads, is empty,
  // cannot be read or is gone. Without a listing it draws nothing, or the one sentence a column ahead with no folders
  // says. Its text stays smaller than the list's. The view reads the listing and reports
  // the pointer, clicks and context requests; the owner decides what they do. It does no I/O.
  class QuietColumnView final : public Flex {
  public:
    // `column` is 0 for column A; `modifiers` reports the keyboard modifiers held, read at a press.
    QuietColumnView(FileIconCache& icons, std::size_t column, std::function<std::uint32_t()> modifiers);
    ~QuietColumnView() override;
    QuietColumnView(const QuietColumnView&) = delete;
    QuietColumnView& operator=(const QuietColumnView&) = delete;
    QuietColumnView(QuietColumnView&&) = delete;
    QuietColumnView& operator=(QuietColumnView&&) = delete;

    // An unchanged serial changes nothing. With the same listing and rowsSerial, only the rows whose marks changed
    // bind again. Another listing or folder starts at the top with no hover and no pending click.
    void setContent(QuietColumnContent content, std::uint64_t serial);
    // The main list's row text size; the column's text stays below it.
    void setListTextSize(float size);

    QuietNotice notice() const noexcept { return m_notice; }
    // True while the column has no listing and no sentence to say: it draws neither its fill nor its header.
    bool blank() const noexcept { return m_blank; }
    // The heights of a row and of the header, as laid out last.
    float rowHeight() const noexcept { return m_rowHeight; }
    float headerHeight() const noexcept { return m_headerHeight; }
    float textSize() const noexcept { return m_textSize; }

    // The bar's share of an entry, 0 to 1, read when its row binds; none, or no callback, shows no bar.
    std::function<std::optional<double>(EntryId)> share;
    // Every pointer motion over the rows, with the row under it (none over empty space), in scene coordinates; also
    // after a scroll or new rows put another row under a resting pointer.
    std::function<void(std::optional<EntryId> row, float sceneX, float sceneY)> onPointerMoved;
    std::function<void()> onPointerLeft;
    // A primary press and release on one row, or a middle press on one; the event names the button.
    std::function<void(const QuietRowEvent&)> onRowClick;
    // The second primary click on one row within the double-click time.
    std::function<void(const QuietRowEvent&)> onRowActivate;
    std::function<void()> onPinControl;
    // A secondary press on a row.
    std::function<void(EntryId entry, float sceneX, float sceneY)> onContextRequest;

  protected:
    void doLayout(Renderer& renderer) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    void fitHeights(Renderer& renderer);
    float m_rowHeight = 0;
    float m_headerHeight = 0;
    class Adapter;
    friend class Adapter;

    std::optional<EntryId> entryAt(std::size_t index) const;
    void refreshRow(std::optional<EntryId> entry);
    void pointerMoved(std::optional<std::size_t> index, float sceneX, float sceneY);
    void pointerLeft();
    void applyHeader();

    std::unique_ptr<Adapter> m_adapter;
    std::size_t m_column;
    QuietColumnContent m_content;
    std::optional<std::uint64_t> m_serial;
    std::size_t m_rowCount = 0;
    QuietNotice m_notice = QuietNotice::None;
    bool m_blank = true;
    float m_textSize;
    std::optional<EntryId> m_hovered;
    std::size_t m_hoveredIndex = 0;
    Label* m_title = nullptr;
    Label* m_count = nullptr;
    Button* m_pin = nullptr;
    VirtualGridView* m_grid = nullptr;
    GridPointer* m_pointer = nullptr;
    Flex* m_header = nullptr;
    Box* m_rule = nullptr;
    Flex* m_noticeBox = nullptr;
    Label* m_noticeLabel = nullptr;
  };
} // namespace luciole
