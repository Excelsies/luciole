#include "formats/sheet_ods.h"

#include "formats/xml_reader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kContent = "content.xml";
    constexpr std::uint64_t kMaxPosition = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) {
      return b > kMaxPosition - a ? kMaxPosition : a + b;
    }

    // An ISO 8601 duration such as "PT18H00M00S" or "-P1DT2H" in days; years and months are refused. Each pass
    // consumes a designator.
    std::optional<double> parseDuration(std::string_view text) {
      const bool negative = text.starts_with('-');
      text.remove_prefix(negative ? 1 : 0);
      if (!text.starts_with('P')) {
        return std::nullopt;
      }
      text.remove_prefix(1);
      double days = 0.0;
      bool inTime = false;
      bool any = false;
      while (!text.empty()) {
        if (text.front() == 'T' && !inTime) {
          inTime = true;
          text.remove_prefix(1);
          continue;
        }
        double value = 0.0;
        const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
        const auto used = static_cast<std::size_t>(end - text.data());
        if (status != std::errc{} || used >= text.size() || value < 0.0 || !std::isfinite(value)) {
          return std::nullopt;
        }
        const char unit = text[used];
        double perDay = 0.0;
        if (unit == 'D' && !inTime) {
          perDay = 1.0;
        } else if (inTime && (unit == 'H' || unit == 'M' || unit == 'S')) {
          perDay = unit == 'H' ? 24.0 : (unit == 'M' ? 1'440.0 : 86'400.0);
        } else {
          return std::nullopt;
        }
        days += value / perDay;
        any = true;
        text.remove_prefix(used + 1);
      }
      return any ? std::optional(negative ? -days : days) : std::nullopt;
    }

    // A table cell as its start tag gives it; paragraphs add the text later.
    struct PendingCell {
      SheetCell cell;
      std::uint64_t repeat = 1;
      bool typed = false; // has a value type, so it is not empty even without text
    };

    std::expected<PendingCell, SheetError> readCellStart(std::span<const XmlAttribute> attributes) {
      const auto repeat = parseSheetCount(attributeValue(attributes, "table:number-columns-repeated").value_or("1"));
      const auto type = attributeValue(attributes, "office:value-type");
      const auto value = [attributes](std::string_view name) { return attributeValue(attributes, name).value_or(""); };
      PendingCell pending{
          .cell = {.text = {}, .formula = attributeValue(attributes, "table:formula").has_value()},
          .repeat = repeat.value_or(0),
          .typed = type.has_value(),
      };
      std::optional<double> number = 0.0;
      if (attributeValue(attributes, "calcext:value-type") == "error") {
        pending.cell.kind = SheetCellKind::Error;
      } else if (type == "float" || type == "percentage" || type == "currency") {
        pending.cell.kind = SheetCellKind::Number;
        number = parseSheetNumber(value("office:value"));
      } else if (type == "date") {
        pending.cell.kind = SheetCellKind::Date;
        number = sheetSerialFromIso(value("office:date-value"), DateSystem::Date1900);
      } else if (type == "time") {
        pending.cell.kind = SheetCellKind::Time;
        number = parseDuration(value("office:time-value"));
      } else if (type == "boolean") {
        const std::string_view flag = value("office:boolean-value");
        pending.cell.kind = SheetCellKind::Boolean;
        const bool known = flag == "true" || flag == "1" || flag == "false" || flag == "0";
        number = known ? std::optional(flag == "true" || flag == "1" ? 1.0 : 0.0) : std::nullopt;
      } else if (type && type != "string") {
        number = std::nullopt;
      }
      if (!number || pending.repeat == 0) {
        return sheetFailure(SheetErrorKind::BadValue, kContent);
      }
      pending.cell.number = *number;
      return pending;
    }

    // Walks content.xml: lists every table and stores the rows of the selected one.
    class ContentParser {
    public:
      ContentParser(std::size_t sheetIndex, const SheetLimits& limits, SheetTableBuilder& builder)
          : m_sheetIndex(sheetIndex), m_limits(limits), m_builder(builder) {}

      const std::vector<std::string>& names() const { return m_names; }

      // `depth` is the reader's depth after the event.
      SheetStep visit(const XmlEvent& event, std::size_t depth) {
        if (event.kind == XmlEventKind::StartElement) {
          return start(localName(event.name), event.attributes, depth).transform([] { return true; });
        }
        if (event.kind == XmlEventKind::EndElement) {
          end(depth + 1);
        } else if (m_paragraphDepth && !m_annotationDepth) {
          m_builder.collect(m_pending.cell.text, event.text);
        }
        return true;
      }

    private:
      std::expected<void, SheetError>
      start(std::string_view local, std::span<const XmlAttribute> attributes, std::size_t depth) {
        if (!m_tableDepth) {
          if (local == "table") {
            m_names.emplace_back(attributeValue(attributes, "table:name").value_or(""));
            m_tableDepth = depth;
            m_reading = m_names.size() - 1 == m_sheetIndex;
            m_row = 0;
          }
          return {};
        }
        if (!m_reading || m_annotationDepth) {
          return {};
        }
        if (!m_rowDepth) {
          if (local == "table-row") {
            const auto repeat = parseSheetCount(attributeValue(attributes, "table:number-rows-repeated").value_or("1"));
            if (!repeat || *repeat == 0) {
              return sheetFailure(SheetErrorKind::BadValue, kContent);
            }
            m_rowDepth = depth;
            m_rowRepeat = *repeat;
            m_column = 0;
            m_cells = SheetRow{};
          }
          return {};
        }
        if (!m_cellDepth) {
          if (local == "table-cell" || local == "covered-table-cell") {
            auto pending = readCellStart(attributes);
            if (!pending) {
              return std::unexpected(pending.error());
            }
            m_pending = std::move(*pending);
            m_cellDepth = depth;
            m_paragraphs = 0;
          }
          return {};
        }
        return startInCell(local, attributes, depth);
      }

      std::expected<void, SheetError>
      startInCell(std::string_view local, std::span<const XmlAttribute> attributes, std::size_t depth) {
        std::string& text = m_pending.cell.text;
        if (local == "annotation") {
          m_annotationDepth = depth;
        } else if ((local == "p" || local == "h") && !m_paragraphDepth) {
          m_builder.collect(text, m_paragraphs++ > 0 ? "\n" : "");
          m_paragraphDepth = depth;
        } else if (m_paragraphDepth && local == "s") {
          const auto count = parseSheetCount(attributeValue(attributes, "text:c").value_or("1"));
          if (!count) {
            return sheetFailure(SheetErrorKind::BadValue, kContent);
          }
          // One space past the character limit is enough for the clip to mark the cell.
          m_builder.collect(text, std::string(std::min(*count, saturatingAdd(m_limits.maxCellChars, 1)), ' '));
        } else if (m_paragraphDepth && (local == "tab" || local == "line-break")) {
          m_builder.collect(text, local == "tab" ? "\t" : "\n");
        }
        return {};
      }

      // `elementDepth` is the depth the element had while open.
      void end(std::size_t elementDepth) {
        if (m_annotationDepth == elementDepth) {
          m_annotationDepth.reset();
        } else if (m_paragraphDepth == elementDepth) {
          m_paragraphDepth.reset();
        } else if (m_cellDepth == elementDepth) {
          m_cellDepth.reset();
          endCell();
        } else if (m_rowDepth == elementDepth) {
          m_rowDepth.reset();
          endRow();
        } else if (m_tableDepth == elementDepth) {
          m_tableDepth.reset();
          m_reading = false;
        }
      }

      // Copies a cell with content to each column it repeats over, up to one past the column limit so the table
      // is marked truncated; an empty cell only moves the column. Only Text and Error cells keep their text.
      void endCell() {
        const std::uint64_t column = m_column;
        m_column = saturatingAdd(m_column, m_pending.repeat);
        if (!m_pending.typed && m_pending.cell.text.empty()) {
          return;
        }
        if (m_pending.cell.kind != SheetCellKind::Text && m_pending.cell.kind != SheetCellKind::Error) {
          m_pending.cell.text.clear();
        }
        const std::uint64_t room = m_limits.maxColumns > column ? m_limits.maxColumns - column : 0;
        const std::uint64_t copies = std::min(m_pending.repeat, saturatingAdd(room, 1));
        for (std::uint64_t k = 0; k < copies; ++k) {
          m_builder.addCell(m_cells, column + k, m_pending.cell);
        }
      }

      // Copies a row with cells to each index it repeats over, up to one past the row limit or until the table is
      // full; an empty row only moves the index.
      void endRow() {
        const std::uint64_t row = m_row;
        m_row = saturatingAdd(m_row, m_rowRepeat);
        if (m_cells.cells.empty()) {
          return;
        }
        const std::uint64_t room = m_limits.maxRows > row ? m_limits.maxRows - row : 0;
        const std::uint64_t copies = std::min(m_rowRepeat, saturatingAdd(room, 1));
        for (std::uint64_t k = 0; k < copies && !m_builder.full(); ++k) {
          m_builder.addRow(row + k, m_cells);
        }
      }

      std::size_t m_sheetIndex;
      SheetLimits m_limits;
      SheetTableBuilder& m_builder;
      std::vector<std::string> m_names;
      std::optional<std::size_t> m_tableDepth;
      std::optional<std::size_t> m_rowDepth;
      std::optional<std::size_t> m_cellDepth;
      std::optional<std::size_t> m_paragraphDepth;
      std::optional<std::size_t> m_annotationDepth;
      std::uint64_t m_row = 0;
      std::uint64_t m_rowRepeat = 1;
      std::uint64_t m_column = 0;
      std::size_t m_paragraphs = 0;
      SheetRow m_cells;
      PendingCell m_pending;
      bool m_reading = false;
    };
  } // namespace

  std::expected<SheetBook, SheetError>
  readOds(const MemberReader& members, std::size_t sheetIndex, const SheetLimits& limits) {
    const auto bytes = readSheetMember(members, kContent);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    SheetTableBuilder builder(limits);
    ContentParser parser(sheetIndex, limits, builder);
    const auto read = visitSheetXml(*bytes, kContent, [&parser](const XmlEvent& event, std::size_t depth) {
      return parser.visit(event, depth);
    });
    if (!read) {
      return std::unexpected(read.error());
    }
    if (sheetIndex >= parser.names().size()) {
      return sheetFailure(SheetErrorKind::SheetIndex, kContent);
    }
    return SheetBook{
        .sheetNames = parser.names(), .sheetIndex = sheetIndex, .table = builder.finish(DateSystem::Date1900)
    };
  }
} // namespace luciole::formats
