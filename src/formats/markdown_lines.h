#pragma once

#include "formats/markdown_text.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The tests that decide which block a line starts: fences, ATX headings, rules, list markers and quotes.
namespace luciole::formats::markdown {
  // Spaces of indentation a block marker may have.
  inline constexpr std::size_t kMaxIndent = 3;

  struct Fence {
    char ch = '`';
    std::size_t count = 0;
    std::size_t indent = 0;
    Line lang;
  };

  // The code fence a line opens: three or more backquotes or tildes, with the first word after them as the
  // language. A backquote fence takes no backquote in that text.
  std::optional<Fence> fenceOf(const Line& line);

  struct Atx {
    std::uint8_t level = 1;
    Line text;
  };

  // The ATX heading a line holds: one to six '#' and a space or the line end, then its text without a closing run
  // of '#'.
  std::optional<Atx> atxOf(const Line& line);

  // Three or more of one of '-', '*' or '_', with only spaces or tabs between them.
  bool isRule(std::string_view line);

  // A line of nothing but `ch` around white space, indented at most kMaxIndent.
  bool allOf(std::string_view line, char ch);

  struct Marker {
    std::size_t indent = 0;
    bool ordered = false;
    std::uint32_t number = 0;
    char bullet = '-';
    std::size_t content = 0; // column where the item's text starts
  };

  // The list marker a line starts with: '-', '*' or '+', or up to nine digits and '.' or ')', then a space or the
  // line end.
  std::optional<Marker> markerOf(std::string_view line);

  // A '>' indented at most kMaxIndent.
  bool startsQuote(std::string_view line);

  // Whether a line ends a paragraph: a fence, a heading, a quote, a rule, or a list item with text that is not an
  // ordered item numbered other than 1.
  bool interrupts(const Line& line);
} // namespace luciole::formats::markdown
