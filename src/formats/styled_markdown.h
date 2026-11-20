#pragma once

#include "formats/file_kind.h"
#include "formats/styled_text.h"

#include <cstddef>
#include <string_view>
#include <vector>

// What the builders of styledText share, and the Markdown builder.
namespace luciole::formats::styled {
  // The document being built and what is left of its limits.
  struct Builder {
    StyledDocument& document;
    std::size_t columns = kStyledLineColumns;
    std::size_t linesLeft = kStyledTextLines;
    std::size_t colorsLeft = kMaxColorsPerFile;
  };

  // Appends text to the runs, joining the last run when its style matches; empty text adds nothing.
  void appendRun(std::vector<StyledRun>& runs, std::string_view text, StyleRole role, bool italic, bool bold);

  // Adds a line to the block, cut at the column limit unless it is prose, with the swatches of its color codes in this
  // kind. False, and the document marked as capped, when the line limit is used up.
  bool addLine(Builder& builder, StyledBlock& block, std::vector<StyledRun>&& runs, const FileKind& kind);

  // Adds the lines of text, split at LF, CRLF and CR, tokenized by `syntax` (JSON line by line); stops at the line
  // limit. Swatches follow the kind.
  void
  addCodeLines(Builder& builder, StyledBlock& block, std::string_view text, const FileKind& kind, TextSyntax syntax);

  // Adds the blocks of Markdown text.
  void addMarkdown(Builder& builder, std::string_view text);
} // namespace luciole::formats::styled
