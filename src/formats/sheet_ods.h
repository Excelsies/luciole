#pragma once

#include "formats/sheet.h"

#include <cstddef>
#include <expected>

namespace luciole::formats {
  // Reads an ODS spreadsheet from its content.xml member: the table names in document order and the table at
  // `sheetIndex`. Repeated empty rows and cells only advance the position; repeated rows and cells with content
  // are copied up to the limits. Values come from the cell's value type: float, percentage and currency as
  // Number, date as Date (a serial of the 1900 system, any time zone passed over), time as Time, boolean, and
  // string or no type as the cell's paragraphs joined with newlines; only Text and Error cells keep text.
  // Elements are matched by local name and attributes by their standard prefixes (office, table, text and
  // calcext).
  std::expected<SheetBook, SheetError>
  readOds(const MemberReader& members, std::size_t sheetIndex = 0, const SheetLimits& limits = {});
} // namespace luciole::formats
