#pragma once

#include "formats/sheet.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace luciole::formats {
  // How an XLSX cell format shows a number. Clock shows the time of day; Elapsed shows a duration.
  enum class XlsxFormat : std::uint8_t { Number, Date, Clock, Elapsed };

  // The kind of each cell format (cellXfs) in a styles part, from its number format id. Built-in ids 14 to 17
  // and 22 show dates; 18 to 21, 45 and 47 the time of day; 46 elapsed time. A custom code is a Date when it shows
  // a year, a day, or a month without hours, seconds or elapsed time; Elapsed when it has an [h], [m] or [s] token;
  // Clock when it shows only hours, minutes or seconds. At most 65,536 cell formats are kept.
  std::expected<std::vector<XlsxFormat>, SheetError> parseXlsxStyles(std::string_view bytes, std::string_view member);
} // namespace luciole::formats
