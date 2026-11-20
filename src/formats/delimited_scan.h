#pragma once

#include "formats/delimited_table.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Delimited text read record by record, for the reader, the dialect detector and the writers.
namespace luciole::formats::delimited {
  inline constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

  struct Syntax {
    char delimiter = ',';
    char quote = '"';
    bool backslash = false;
  };

  enum class Terminator : std::uint8_t { None, Lf, CrLf, Cr };

  // One record: [begin, contentEnd) holds its cells, [contentEnd, end) its line ending.
  struct RecordScan {
    std::size_t begin = 0;
    std::size_t contentEnd = 0;
    std::size_t end = 0;
    std::size_t fieldCount = 0;
    Terminator terminator = Terminator::None;
    bool unterminated = false;
  };

  // The delimiter, quote and escape of a dialect as bytes.
  Syntax syntaxOf(const Dialect& dialect);

  // Where the first record starts: after the byte order mark when the dialect has one and the bytes start with it.
  std::size_t contentStart(std::string_view bytes, const Dialect& dialect);

  // An LF or a CR.
  bool isLineBreak(char c);

  // The record starting at `pos`, storing at most `maxStore` of its cells in `cells`. A position at or past the end
  // gives an empty record there.
  RecordScan scanRecord(
      std::string_view bytes, std::size_t pos, const Syntax& syntax, std::vector<Cell>& cells, std::size_t maxStore
  );
} // namespace luciole::formats::delimited
