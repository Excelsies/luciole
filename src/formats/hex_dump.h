#pragma once

#include "formats/styled_text.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace luciole::formats {
  inline constexpr std::size_t kHexDumpBytes = 64U * 1024U;
  inline constexpr std::size_t kHexDumpRowBytes = 16;
  // Bytes per row a dump offers, widest first.
  inline constexpr std::array<std::size_t, 3> kHexDumpRowChoices{16, 12, 8};

  // Bytes as one code block of rows of `rowBytes`: an eight-digit hex offset, the bytes in lowercase hex in groups
  // of eight, and the bytes between bars with a dot for every byte outside printable ASCII. A count between the
  // choices takes the widest choice below it, and one below 8 takes 8. Offsets are comments, bytes numbers with zero
  // bytes and the dots as punctuation. At most kHexDumpBytes are shown; bytesCapped says more were given.
  StyledDocument hexDump(std::string_view bytes, std::size_t rowBytes = kHexDumpRowBytes);

  // Characters in a full row of `rowBytes`, one of the choices.
  std::size_t hexDumpRowWidth(std::size_t rowBytes);
  // The widest choice whose full rows fit in `columns` characters; the narrowest when none does or for NaN.
  std::size_t hexDumpRowsFitting(double columns);

  // The bytes per row of a hexDump document, read from its first row; 0 for one without rows or not made by hexDump.
  std::size_t hexDumpRowBytes(const StyledDocument& dump);
  // The bytes of a hexDump document read back from its rows and dumped again in rows of `rowBytes`, cut as the
  // document was; none for a document hexDump did not make.
  std::optional<StyledDocument> hexDumpAgain(const StyledDocument& dump, std::size_t rowBytes);
} // namespace luciole::formats
