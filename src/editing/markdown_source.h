#pragma once

#include "formats/styled_text.h"

#include <cstddef>
#include <string_view>
#include <vector>

// Markdown source colored line by line, as written: markers keep their place and nothing is rendered.
namespace luciole::editing {
  // The code fence open at the start of a line; count is 0 when none is.
  struct MarkdownFence {
    char ch = '\0';
    std::size_t count = 0;
    bool operator==(const MarkdownFence&) const = default;
  };

  // The fence open after `line`, given the one open before it.
  MarkdownFence fenceAfter(std::string_view line, MarkdownFence fence);

  // Runs covering every byte of `line` in order. Inside a fence the line is InlineCode; fence markers are
  // Punctuation. Elsewhere: ATX heading lines are Heading, rules and setext underlines Punctuation, quote markers
  // Quote, list markers Punctuation, then code spans InlineCode, emphasis (italic) and strong (bold) with their
  // markers, inline links, images and autolinks Link. Emphasis closes only on a whole marker run of its own length
  // (three or more count as three), so a styled range never ends inside a run; emphasis nested in emphasis, fences
  // inside block quotes and indented code blocks are not recognized. Work per line is at most its length times
  // kMaxLabel plus kMaxTarget bytes for links, plus its length times the square root of its length for code spans.
  std::vector<formats::StyledRun> markdownRuns(std::string_view line, MarkdownFence fence);
} // namespace luciole::editing
