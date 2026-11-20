#pragma once

#include "formats/markdown.h"
#include "formats/markdown_text.h"

#include <cstddef>

// Inline Markdown: emphasis, strong, code spans, links, images, escapes and hard breaks.
namespace luciole::formats::markdown {
  // The runs of one inline text. Link labels are parsed as inline text down to kMaxLabelDepth levels.
  Range parseInline(Context& context, const Source& source, std::size_t labelDepth);

  // The inline source of one line.
  Source sourceOf(const Line& line);
} // namespace luciole::formats::markdown
