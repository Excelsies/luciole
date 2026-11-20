#include "ui/controls/table_cell_editor.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "render/scene/input_area.h"
#include "ui/controls/input.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace luciole {
  namespace {
    // The inset Input keeps between its padding and its text; the field's padding leaves it out, so the text stays
    // where the cell showed it.
    constexpr float kInputTextInset = 3;
  } // namespace

  TableCellEditor::TableCellEditor() {
    setClipChildren(true);
    setVisible(false);
    auto field = std::make_unique<Input>();
    field->setFrameRadius(Style::radiusSm);
    field->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t modifiers) { return fieldKey(sym, modifiers); });
    field->setOnFocusLoss([this] { close(TableFieldExit::Stay); });
    m_field = static_cast<Input*>(addChild(std::move(field)));
  }

  TableCellEditor::~TableCellEditor() = default;

  InputArea& TableCellEditor::fieldArea() const noexcept { return *m_field->inputArea(); }

  void TableCellEditor::open(TableCell cell, std::string_view text, std::string original) {
    m_cell = cell;
    m_original = std::move(original);
    m_field->setValue(text);
    setVisible(true);
    markLayoutDirty();
  }

  void TableCellEditor::close(TableFieldExit exit) {
    if (!m_cell) {
      return;
    }
    const TableCell cell = *m_cell;
    m_cell.reset();
    setVisible(false);
    std::optional<std::string> text;
    if (exit != TableFieldExit::Cancel && m_field->value() != m_original) {
      text = m_field->value();
    }
    m_original.clear();
    if (onExit) {
      onExit(exit, cell, std::move(text));
    }
  }

  void TableCellEditor::setTextSize(float size) { m_field->setFontSize(size); }

  void TableCellEditor::place(const LayoutRect& cell, float padding) {
    m_field->setControlHeight(cell.height);
    m_field->setHorizontalPadding(std::max(0.0F, padding - kInputTextInset));
    if (cell.x != m_rect.x || cell.y != m_rect.y || cell.width != m_rect.width || cell.height != m_rect.height) {
      m_rect = cell;
      markLayoutDirty();
    }
  }

  void TableCellEditor::doLayout(Renderer& renderer) {
    if (m_cell) {
      m_field->arrange(renderer, m_rect);
    }
  }

  // Enter, Tab and Escape leave the field; while text is being composed they belong to the composition.
  bool TableCellEditor::fieldKey(std::uint32_t sym, std::uint32_t modifiers) {
    if (!m_cell || m_field->composing()) {
      return false;
    }
    const bool shift = (modifiers & KeyMod::Shift) != 0;
    const bool plain = (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0;
    if (KeySymbol::isEnter(sym) && plain) {
      close(TableFieldExit::Down);
    } else if (KeySymbol::isTab(sym) && plain) {
      close(shift || sym == XKB_KEY_ISO_Left_Tab ? TableFieldExit::Left : TableFieldExit::Right);
    } else if (KeySymbol::isEscape(sym)) {
      close(TableFieldExit::Cancel);
    } else {
      return false;
    }
    return true;
  }
} // namespace luciole
