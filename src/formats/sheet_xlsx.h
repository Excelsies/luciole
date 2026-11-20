#pragma once

#include "formats/sheet.h"

#include <cstddef>
#include <expected>

namespace luciole::formats {
  // Reads an XLSX workbook from its archive members: the sheet names in workbook order, the date system, and the
  // sheet at `sheetIndex` as a table. The workbook is read from xl/workbook.xml and its relationships from
  // xl/_rels/workbook.xml.rels, which lead to its sheets, shared strings and styles; the last two are optional.
  // A relationship target is resolved from xl/, or from the archive root when it starts with '/'. Numbers whose
  // cell format shows a date are reported as Date; a time-of-day format gives a Time of the value's fraction of a
  // day, and an elapsed-time format a Time of the whole value. A "d" cell holding only a time is a Time. Element
  // names are matched by local name.
  std::expected<SheetBook, SheetError>
  readXlsx(const MemberReader& members, std::size_t sheetIndex = 0, const SheetLimits& limits = {});
} // namespace luciole::formats
