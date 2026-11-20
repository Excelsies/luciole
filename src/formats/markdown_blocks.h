#pragma once

#include "formats/markdown.h"
#include "formats/markdown_text.h"

#include <cstddef>
#include <span>

// Markdown blocks: paragraphs, headings, code, quotes, lists, tables and rules.
namespace luciole::formats::markdown {
  // The blocks of the lines, stored in the document. Quotes and list items nest down to kMaxDepth; deeper text
  // becomes one plain paragraph.
  Range parseBlocks(Context& context, std::span<const Line> lines, std::size_t depth);
} // namespace luciole::formats::markdown
