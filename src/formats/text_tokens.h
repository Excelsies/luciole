#pragma once

#include "formats/file_kind.h"
#include "formats/language_keywords.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::formats {
  inline constexpr std::size_t kMaxTextLines = 2000;
  inline constexpr std::size_t kMaxLineWidth = 400; // code points, before the cut marker
  inline constexpr std::size_t kMaxJsonDepth = 256;

  // Added, Removed, Hunk and Header are whole diff lines. Space and More own no bytes of the text: Space stands for
  // `length` spaces the JSON printer inserts at `offset`, More for the ellipsis after a line cut at the width limit.
  enum class TokenClass : std::uint8_t {
    Plain,
    Key,
    String,
    Number,
    Literal,
    Comment,
    Punct,
    Tag,
    Attr,
    Keyword,
    Added,
    Removed,
    Hunk,
    Header,
    Space,
    More
  };

  struct Token {
    std::uint32_t offset = 0; // bytes into the formatted text
    std::uint32_t length = 0;
    TokenClass tokenClass = TokenClass::Plain;
    bool operator==(const Token&) const = default;
  };

  using TextLine = std::vector<Token>;

  struct FormattedText {
    std::vector<TextLine> lines; // at most kMaxTextLines, each at most kMaxLineWidth code points plus a More token
    std::optional<std::size_t> totalLines; // lines in the text; empty when JSON printing stopped at the line limit
    bool linesCapped = false;
    std::size_t widthCut = 0; // lines cut at the width limit
    // why the text is not printable JSON (invalid, or nested deeper than kMaxJsonDepth), or "unknown language"
    std::optional<std::string> error;
  };

  // Splits text at LF, CRLF and CR and colors each line by the language's tokenizer; the comment style applies to
  // code. JSON is validated and printed with two-space indentation, keeping key order and the source text of every
  // number; invalid or too deeply nested JSON gives the raw lines as plain tokens and the reason in `error`. Code
  // keywords come from keywords(language). Only the first 4 GiB of text are read. In invalid UTF-8 a continuation
  // byte that no lead byte announced counts as one code point, so a line never exceeds 4 * kMaxLineWidth bytes.
  FormattedText formatText(std::string_view text, Language language, CommentStyle comment = CommentStyle::None);

  // The token's bytes, its spaces (at most 2 * kMaxJsonDepth) or the ellipsis. Bytes past the text are left out.
  std::string tokenText(std::string_view text, const Token& token);
  std::string lineText(std::string_view text, const TextLine& line);
} // namespace luciole::formats
