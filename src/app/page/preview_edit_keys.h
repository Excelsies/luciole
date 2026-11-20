#pragma once

#include <cstdint>

namespace luciole {
  // Whether the key chord is a page shortcut that leaves the file being edited: a view mode (Ctrl+1 to Ctrl+4) or
  // another folder (Alt+Left, Alt+Right, Alt+Up, Alt+Home). While an editor or a cell field has the keyboard it
  // passes these on to the page, which acts as for the matching button and asks about unsaved changes when the file
  // is left. Every other key stays with the editor.
  bool leavesEditedFile(std::uint32_t sym, std::uint32_t modifiers);

  // Whether a text field keeps the key from the page's shortcuts: the location field keeps every key, and an editor
  // with the keyboard keeps every key that does not leave its file.
  bool textHasKey(bool locationFocused, bool editorFocused, std::uint32_t sym, std::uint32_t modifiers);
} // namespace luciole
