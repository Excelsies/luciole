#pragma once

#include "app/page/page_context.h"
#include "app/page/preview_edit_controller.h"

#include <cstdint>
#include <functional>
#include <memory>

// The column preview's editor for `page`: its question is a dialog of the page with Save focused, it takes the
// page's keyboard and clipboard, and it asks the page's surface for frames. `modifiers` reports the keyboard
// modifiers held.
std::unique_ptr<luciole::PreviewEditController>
pagePreviewEditor(PageContext& page, std::function<std::uint32_t()> modifiers);

// The callbacks the page registers with PageContext::setEditing for `editor`, which outlives them.
PageEditing pageEditing(luciole::PreviewEditController& editor);
