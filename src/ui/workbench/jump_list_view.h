#pragma once

#include "model/directory_options.h"
#include "ui/controls/flex.h"
#include "workbench/jump_rank.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class InputArea;
class Label;
class VirtualGridView;

namespace luciole {
  class GridPointer;

  enum class JumpNotice : std::uint8_t { None, Empty, Measuring, NotMeasured };

  struct JumpListContent {
    workbench::JumpList list;
    workbench::NodeId linked = workbench::kNoNode; // the row another pane points at
    bool notMeasured = false;                      // the location is not a local folder, so no row can exist
  };

  // The Jump ahead list: a virtualized row per folder worth jumping to, with its name, its path below the current
  // folder, its value and a share bar. Up, Down, Home, End and the page keys move a cursor, Enter or Space activate
  // it; a click follows the user's activation setting, and a middle click is reported on its own. With no rows it
  // says whether there is nothing to show or the folder is still being measured. It does no I/O.
  class JumpListView final : public Flex {
  public:
    JumpListView();
    ~JumpListView() override;
    JumpListView(const JumpListView&) = delete;
    JumpListView& operator=(const JumpListView&) = delete;
    JumpListView(JumpListView&&) = delete;
    JumpListView& operator=(JumpListView&&) = delete;

    // An unchanged serial changes nothing; otherwise only rows that differ, or whose link changed, bind again. The
    // cursor stays on its folder's row, or at its place within the rows when that row is gone.
    void setContent(JumpListContent content, std::uint64_t serial);
    // Single click activates a row; double click moves the cursor on the first click and activates on the second.
    void setActivationMode(ActivationMode mode) { m_activationMode = mode; }

    JumpNotice notice() const noexcept { return m_notice; }
    std::optional<std::size_t> cursor() const noexcept { return m_cursor; }
    // Takes the keys; the owner gives it focus.
    InputArea* focusArea() const noexcept;

    // The row under the pointer changed, by motion, a scroll or new rows; null when there is none. The row lives for
    // the call.
    std::function<void(const workbench::JumpRow*)> onHover;
    std::function<void(const workbench::JumpRow&)> onActivate;
    // A middle press on a row; the cursor stays where it is.
    std::function<void(const workbench::JumpRow&)> onMiddleClick;

    // The main list's icon size (0 for the default): the rows' height follows it.
    void setListIconSize(int size);

  protected:
    void doLayout(Renderer& renderer) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    void fitRowHeight(Renderer& renderer);
    int m_listIcon = 0;

    class Adapter;
    friend class Adapter;

    bool key(std::uint32_t sym, std::uint32_t modifiers);
    void moveCursor(std::size_t index);
    void activate(std::size_t index);
    void hover(std::optional<std::size_t> index);
    void refresh(std::optional<std::size_t> index);

    std::unique_ptr<Adapter> m_adapter;
    JumpListContent m_content;
    std::optional<std::uint64_t> m_serial;
    JumpNotice m_notice = JumpNotice::Empty;
    ActivationMode m_activationMode = ActivationMode::DoubleClick;
    std::optional<std::size_t> m_cursor;
    std::optional<std::size_t> m_hovered;
    VirtualGridView* m_grid = nullptr;
    GridPointer* m_pointer = nullptr;
    Flex* m_noticeBox = nullptr;
    Label* m_noticeLabel = nullptr;
  };
} // namespace luciole
