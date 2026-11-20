#pragma once

#include "formats/text_tokens.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Text cut into lines of colored tokens: the pieces every tokenizer of formatText shares.
namespace luciole::formats::tokenize {
  inline constexpr std::string_view kJsonSpace = " \t\r\n";
  inline constexpr std::string_view kJsonStop = " \t\r\n,:[]{}\"";

  // A double quote, a single quote or a backquote.
  bool isQuote(char c);

  // Bytes taken by the first `count` code points of text.
  std::size_t prefixBytes(std::string_view text, std::size_t count);

  // Code points in text; stops counting once past `limit`. A continuation byte that no lead byte announced counts as
  // one code point.
  std::size_t codePoints(std::string_view text, std::size_t limit);

  // Writes tokens for bytes of one line, or of its tail, into the line's token list.
  struct LineWriter {
    std::string_view s;
    std::size_t base; // offset of s in the text
    TextLine* out;

    // Joins the previous token when it has the same class and ends where this one starts; keys stay apart.
    void add(std::size_t begin, std::size_t end, TokenClass tokenClass) const {
      if (end <= begin) {
        return;
      }
      const auto offset = static_cast<std::uint32_t>(base + begin);
      const auto length = static_cast<std::uint32_t>(end - begin);
      if (!out->empty()) {
        auto& last = out->back();
        if (last.tokenClass == tokenClass && tokenClass != TokenClass::Key && last.offset + last.length == offset) {
          last.length += length;
          return;
        }
      }
      out->push_back({.offset = offset, .length = length, .tokenClass = tokenClass});
    }

    LineWriter tail(std::size_t from) const { return {.s = s.substr(from), .base = base + from, .out = out}; }
  };

  struct Block {
    std::string_view end;
    TokenClass tokenClass = TokenClass::Plain;
  };

  // What carries from one line to the next.
  struct ScanState {
    std::optional<Block> block; // open comment, multi-line string or CDATA
    bool inTag = false;
    std::size_t oldLeft = 0; // lines still expected in the current diff hunk
    std::size_t newLeft = 0;
  };

  // End (exclusive) of the quoted string at i, honoring backslash escapes; an unclosed string runs to the end, and an
  // index at or past the end gives the end.
  std::size_t stringEnd(std::string_view s, std::size_t i);

  // Continues the open block from i and returns the index after it, or the line end; i itself when no block is open.
  std::size_t inBlock(const LineWriter& w, std::size_t i, ScanState& state);

  // [-+]? (0x[0-9a-f_]+ | digit [digit_]* (.digits)? ([eE][-+]?digits)?), ignoring case.
  bool isNumber(std::string_view w);

  // The first index at or after i that is not JSON white space.
  std::size_t skipJsonSpace(std::string_view s, std::size_t i);

  // Line ends in place, at most kMaxTextLines kept, each cut to kMaxLineWidth + 1 code points.
  struct LineSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  struct SplitLines {
    std::vector<LineSpan> lines;
    std::size_t total = 0;
  };

  // A final line break ends the last line; it does not start another. Empty text is one empty line.
  SplitLines splitLines(std::string_view text);

  // Cuts a line to kMaxLineWidth code points and appends a More token. Returns true when it had to. A token past the
  // end of text counts as empty.
  bool cutWidth(std::string_view text, TextLine& line);
} // namespace luciole::formats::tokenize
