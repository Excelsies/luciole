#pragma once

#include "formats/delimited_table.h"

#include <cstddef>
#include <optional>
#include <string_view>

// Finding the dialect of delimited text: delimiter, quote, escape, line ending, header row and byte order mark.
namespace luciole::formats {
  // Dialect detection reads at most this many bytes after the byte order mark.
  inline constexpr std::size_t kDialectSampleBytes = 64 * 1024;

  // Choices the caller already knows, for example a tab from a .tsv name or a header the user picked.
  struct DialectHint {
    std::optional<Delimiter> delimiter = std::nullopt;
    std::optional<bool> header = std::nullopt;
  };

  // Detects the dialect from the first kDialectSampleBytes. Hints replace the detected delimiter or header.
  // Empty input gives the default dialect.
  Dialect detectDialect(std::string_view bytes, const DialectHint& hint = {});
} // namespace luciole::formats
