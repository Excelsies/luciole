#pragma once

#include "formats/file_kind.h"
#include "formats/language_keywords.h"
#include "formats/text_lines.h"

#include <cstddef>
#include <cstdint>

// Tokens of a line of code, and of the values of YAML, TOML and INI lines.
namespace luciole::formats::tokenize {
  enum class Family : std::uint8_t { Yaml, Toml, Ini, Code };

  struct CodeOptions {
    CommentStyle style = CommentStyle::Slash;
    Family family = Family::Code;
    bool triple = false; // """ and ''' open multi-line strings
    bool blocks = false; // /* opens a block comment
    KeywordSet keywords{};
  };

  // Strings, numbers, literals, keywords and comments of a value or a line of code.
  void scanCode(const LineWriter& w, ScanState& state, const CodeOptions& options);

  // "key SEP value", where sep is the separator's index; white space before the separator is not part of the key.
  // A separator past the end of the line makes the whole line the key.
  void keyValue(const LineWriter& w, std::size_t sep, const CodeOptions& options, ScanState& state);
} // namespace luciole::formats::tokenize
