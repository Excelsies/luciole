#pragma once

#include "editing/text_buffer.h"

#include <cstddef>
#include <string_view>

// Cursor motions over a TextBuffer. Functions taking a TextBuffer clamp their input position like
// TextBuffer::setSelection, and lineRange clamps its line index. Each costs at most a pass over the line it starts
// on. Up, down and page moves belong to the view.
//
// Clusters approximate extended grapheme clusters: a base code point followed by any number of
//   combining marks U+0300-U+036F, U+1AB0-U+1AFF, U+1DC0-U+1DFF, U+20D0-U+20FF, U+FE20-U+FE2F,
//   variation selectors U+FE00-U+FE0F and U+E0100-U+E01EF,
//   emoji modifiers U+1F3FB-U+1F3FF,
//   a zero width joiner U+200D with the code point after it;
// and a regional indicator U+1F1E6-U+1F1FF takes the one that follows it as a flag pair.
//
// Word characters are ASCII letters, digits and underscore, and every other non-ASCII code point except white
// space (formats/unicode_space.h) and this punctuation: U+00A1-U+00BF other than U+00AA, U+00B5 and U+00BA;
// U+00D7; U+00F7; U+2010-U+2027; U+2030-U+205E; U+3001-U+3003; U+3008-U+3011; U+FF01-U+FF0F; U+FF1A-U+FF20;
// U+FF3B-U+FF40; U+FF5B-U+FF65. A cluster takes the class of its first code point.
namespace luciole::editing {
  // The end of the cluster that starts at `byte` in valid UTF-8 text; `byte` itself at the end.
  std::size_t clusterEnd(std::string_view text, std::size_t byte);
  // The start of the cluster before `byte`, counted from the start of the text; 0 at the start.
  std::size_t clusterStart(std::string_view text, std::size_t byte);

  // One cluster left or right; from a line edge onto the neighboring line's edge.
  TextPosition clusterLeft(const TextBuffer& buffer, TextPosition position);
  TextPosition clusterRight(const TextBuffer& buffer, TextPosition position);
  // Past any blanks, then past one run of word characters or of punctuation. A line edge is one stop.
  TextPosition wordLeft(const TextBuffer& buffer, TextPosition position);
  TextPosition wordRight(const TextBuffer& buffer, TextPosition position);
  // The run of word characters, punctuation or white space at the position, for a double click. At the line end it
  // is the run before the position; on an empty line it is empty.
  TextSelection wordRange(const TextBuffer& buffer, TextPosition position);

  // The first non-blank character of the line, or byte 0 when the position is already there.
  TextPosition lineStart(const TextBuffer& buffer, TextPosition position);
  TextPosition lineEnd(const TextBuffer& buffer, TextPosition position);
  TextPosition documentStart(const TextBuffer& buffer);
  TextPosition documentEnd(const TextBuffer& buffer);
  // From the line start to the next line start, or to the line end on the last line, for a triple click.
  TextSelection lineRange(const TextBuffer& buffer, std::size_t line);
  TextSelection selectAll(const TextBuffer& buffer);
} // namespace luciole::editing
