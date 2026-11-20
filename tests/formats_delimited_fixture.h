#pragma once

#include "formats/delimited_table.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::test::delimited {
  using namespace formats;

  inline constexpr std::string_view kBom = "\xEF\xBB\xBF";

  // The texts of one row's cells.
  inline std::vector<std::string>
  rowTexts(std::string_view bytes, const Dialect& dialect, const Table& table, std::size_t row) {
    std::vector<std::string> texts;
    for (const Cell& cell : rowCells(table, row)) {
      texts.push_back(cellText(bytes, dialect, cell).text);
    }
    return texts;
  }

  using Texts = std::vector<std::string>;
} // namespace luciole::test::delimited
