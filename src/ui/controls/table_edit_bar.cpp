#include "ui/controls/table_edit_bar.h"

#include "ui/controls/button.h"
#include "ui/controls/table_view_cell.h"
#include "ui/style.h"

#include <cmath>
#include <memory>
#include <string_view>

namespace luciole {
  namespace {
    struct EditFace {
      std::string_view glyph;
      std::string_view tooltip;
    };

    constexpr std::array<EditFace, kTableEditCount> kFaces{{
        {.glyph = "row-insert-bottom", .tooltip = "Insert row below"},
        {.glyph = "row-remove", .tooltip = "Delete row"},
        {.glyph = "column-insert-right", .tooltip = "Insert column to the right"},
        {.glyph = "column-remove", .tooltip = "Delete column"},
        {.glyph = "arrow-back-up", .tooltip = "Undo"},
        {.glyph = "arrow-forward-up", .tooltip = "Redo"},
    }};
  } // namespace

  TableEditBar::TableEditBar() {
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setPadding(0, Style::spaceXs);
    for (std::size_t index = 0; index < kTableEditCount; ++index) {
      auto button = std::make_unique<Button>();
      button->setVariant(ButtonVariant::Ghost);
      button->setGlyph(kFaces[index].glyph);
      button->setTooltip(kFaces[index].tooltip);
      button->setOnClick([this, index] {
        if (m_buttons[index]->enabled() && onRequest) {
          onRequest(static_cast<TableEdit>(index));
        }
      });
      m_buttons[index] = static_cast<Button*>(addChild(std::move(button)));
    }
  }

  TableEditBar::~TableEditBar() = default;

  void TableEditBar::setTextSize(float size) {
    if (!std::isfinite(size) || size <= 0) {
      return;
    }
    const float row = std::ceil(size * kTableRowEm);
    for (Button* button : m_buttons) {
      button->setGlyphSize(size);
      button->setControlHeight(row);
      button->setMinWidth(row);
      button->setPadding((row - size) / 2);
    }
    setMinHeight(row);
  }

  void TableEditBar::setAvailable(TableEdit edit, bool available) { button(edit).setEnabled(available); }

  bool TableEditBar::available(TableEdit edit) const { return button(edit).enabled(); }

  Button& TableEditBar::button(TableEdit edit) const { return *m_buttons[static_cast<std::size_t>(edit)]; }
} // namespace luciole
