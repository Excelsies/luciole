#include "app/page/preview_edit_keys.h"

#include "ui/browser/action_shortcuts.h"

namespace luciole {
  bool leavesEditedFile(std::uint32_t sym, std::uint32_t modifiers) {
    using Action = BrowserAction;
    return shortcutAction(
               {Action::Grid, Action::List, Action::Miller, Action::Workbench, Action::Back, Action::Forward,
                Action::Up, Action::Home},
               sym, modifiers
    )
        .has_value();
  }

  bool textHasKey(bool locationFocused, bool editorFocused, std::uint32_t sym, std::uint32_t modifiers) {
    return locationFocused || (editorFocused && !leavesEditedFile(sym, modifiers));
  }
} // namespace luciole
