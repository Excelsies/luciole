#pragma once

#include "ui/controls/flex.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

class Button;

namespace luciole {
  // A change to a table's rows, columns or history that the owner applies.
  enum class TableEdit : std::uint8_t { InsertRowBelow, DeleteRow, InsertColumnRight, DeleteColumn, Undo, Redo };
  inline constexpr std::size_t kTableEditCount = 6;

  // A row of small icon buttons, one per TableEdit, each with a tooltip naming it. The icons are the text size, so
  // the bar is never larger than the table's text.
  class TableEditBar final : public Flex {
  public:
    TableEditBar();
    ~TableEditBar() override;
    TableEditBar(const TableEditBar&) = delete;
    TableEditBar& operator=(const TableEditBar&) = delete;
    TableEditBar(TableEditBar&&) = delete;
    TableEditBar& operator=(TableEditBar&&) = delete;

    void setTextSize(float size);
    // An edit that is not available shows as disabled and reports nothing when pressed.
    void setAvailable(TableEdit edit, bool available);
    bool available(TableEdit edit) const;
    Button& button(TableEdit edit) const;

    // A button was pressed.
    std::function<void(TableEdit edit)> onRequest;

  private:
    std::array<Button*, kTableEditCount> m_buttons{};
  };
} // namespace luciole
