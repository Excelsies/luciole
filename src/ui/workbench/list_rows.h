#pragma once

#include "browser/directory_model.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "thumbnails/file_icon_cache.h"
#include "workbench/jump_rank.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class Box;
class Flex;
class Label;
class Glyph;
class ProgressBar;
class VirtualGridView;

namespace luciole {
  class FileIcon;

  // Text size of column `column` ahead (0 is column A) beside a list whose rows use `listTextSize`: the columns'
  // rule (workbench::columnFont), kept below the list's size. A size that is not finite reads as the body size;
  // others are clamped to 6 to 96 first.
  float quietTextSize(float listTextSize, std::size_t column);

  // The icon of a row of column `column` ahead beside a list whose rows use `listIcon` px icons (0 for the default,
  // which gives the columns' own sizes): the column's size in the list's proportion, at most 40 px.
  float quietIconSize(int listIcon, std::size_t column);
  // The height a row of column `column` takes for that icon: its own height at the default, moving with the icon.
  float quietRowHeight(int listIcon, std::size_t column);
  // The height a Jump ahead row takes beside such a list; the rows hold no icon and move as column A's do.
  float jumpRowHeight(int listIcon);

  // States a row shows besides its entry.
  struct RowMarks {
    bool followed = false; // the next column lists or previews this entry
    bool pinned = false;
    bool linked = false; // another pane points at it
    bool hovered = false;
    bool operator==(const RowMarks&) const = default;
  };

  // A row of a column ahead: a small type icon that never loads a thumbnail, the name, a pin mark and a thin
  // weight bar. Columns further right are quieter: smaller icons, fainter icon and bar.
  class QuietRow final : public Node {
  public:
    QuietRow(FileIconCache& icons, std::size_t column);

    // `share` is the bar's fill from 0 to 1; none, or a value that is not finite, hides the bar.
    void bind(const DirectoryEntry& entry, RowMarks marks, std::optional<double> share, float textSize);
    void release(Renderer& renderer);

    EntryId entry() const noexcept { return m_entry; }
    RowMarks marks() const noexcept { return m_marks; }
    const Label& nameLabel() const noexcept { return *m_name; }
    // The icon's size, from quietIconSize(); a column's own size until set.
    void setIconSize(float size);
    // The bar's fill; none while the bar is hidden.
    std::optional<float> share() const noexcept;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    std::size_t m_column;
    float m_iconSize;
    EntryId m_entry = 0;
    RowMarks m_marks;
    Box* m_background = nullptr;
    Box* m_lead = nullptr;
    Flex* m_row = nullptr;
    FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Glyph* m_pin = nullptr;
    ProgressBar* m_bar = nullptr;
  };

  // The height of a Jump ahead row, which holds two lines of text.
  inline constexpr float kJumpRowHeight = 38;
  // Room above and below a Jump ahead row's two lines.
  inline constexpr float kJumpRowInset = 6;

  // A Jump ahead row: the folder's name over its path below the current folder in smaller text, its value in the
  // measure entries are sized by, and a share bar. A row still being measured is faded.
  class JumpRowTile final : public Node {
  public:
    JumpRowTile();

    // `cursor` draws the keyboard cursor's ring.
    void bind(const workbench::JumpRow& row, RowMarks marks, bool cursor);

    const workbench::JumpRow& row() const noexcept { return m_row; }
    RowMarks marks() const noexcept { return m_marks; }
    bool cursor() const noexcept { return m_cursor; }
    const Label& nameLabel() const noexcept { return *m_name; }
    const Label& pathLabel() const noexcept { return *m_path; }
    const Label& valueLabel() const noexcept { return *m_value; }
    float share() const noexcept;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    workbench::JumpRow m_row;
    RowMarks m_marks;
    bool m_cursor = false;
    Box* m_background = nullptr;
    Flex* m_content = nullptr;
    Label* m_name = nullptr;
    Label* m_path = nullptr;
    Label* m_value = nullptr;
    ProgressBar* m_bar = nullptr;
  };

  // An input area laid over a grid's rows that reports the row under the pointer, in scene coordinates. It takes
  // only the middle button and no wheel, so other presses and scrolling still reach the grid, and it shows the row
  // tooltips the grid's own areas below it would show.
  class GridPointer final : public InputArea {
  public:
    explicit GridPointer(VirtualGridView& grid);

    // Adds a pointer area to `grid`, which owns it.
    static GridPointer& attach(VirtualGridView& grid);
    // Covers the grid's rows again; called after the grid's layout. While the pointer is inside, a scroll that moved
    // another row under it, or a recheck(), reports the row now under it through onMove.
    void fit();
    // The rows changed: the next fit reports the row under the pointer even when its index stayed.
    void recheck() { m_recheck = true; }

    std::function<void(std::optional<std::size_t> row, float sceneX, float sceneY)> onMove;
    std::function<void()> onLeave;
    // A middle press over a row.
    std::function<void(std::size_t row)> onMiddlePress;
    std::function<std::string(std::size_t row)> tooltip;

  private:
    struct ScenePoint {
      float x = 0;
      float y = 0;
    };

    void moved(float localX, float localY);
    void report(std::optional<std::size_t> row);

    VirtualGridView& m_grid;
    std::optional<std::size_t> m_row;
    std::optional<ScenePoint> m_pointer; // while inside
    bool m_recheck = false;
  };

  // A second primary click on the same row within kWindow.
  class DoubleClick {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kWindow{400};

    // True when `key` was clicked less than kWindow before; a double click starts the count again.
    bool click(std::uint64_t key, Clock::time_point now);
    void reset() { m_last.reset(); }

  private:
    std::optional<std::uint64_t> m_last;
    Clock::time_point m_time;
  };
} // namespace luciole
