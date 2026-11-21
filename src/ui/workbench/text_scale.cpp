#include "ui/workbench/text_scale.h"

#include "render/render_context.h"
#include "render/text/system_font.h"
#include "ui/style.h"

namespace luciole {
  float systemTextScale() {
    return RenderContext::clampTextScale(text::currentSystemFont().textScale(Style::fontSizeBody));
  }
} // namespace luciole
