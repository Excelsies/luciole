#include "formats/sheet_xlsx.h"

#include "formats/ascii.h"
#include "formats/sheet_xlsx_styles.h"
#include "formats/utf8.h"
#include "formats/xml_reader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kWorkbook = "xl/workbook.xml";
    constexpr std::string_view kWorkbookRelationships = "xl/_rels/workbook.xml.rels";
    // Relationship targets of the workbook are relative to its folder.
    constexpr std::string_view kWorkbookFolder = "xl/";
    // Excel's last column (XFD) and row, both counted from 1.
    constexpr std::uint64_t kColumnCount = 16'384;
    constexpr std::uint64_t kRowCount = 1'048'576;
    constexpr std::size_t kMaxColumnLetters = 3;
    // "_xHHHH_" escapes a character in shared and inline strings.
    constexpr std::size_t kEscapeBytes = 7;

    enum class CellType : std::uint8_t { Number, SharedString, InlineString, FormulaString, Boolean, Error, Date };
    // The names of the cell types in the order of CellType.
    constexpr std::array<std::string_view, 7> kCellTypeNames = {"n", "s", "inlineStr", "str", "b", "e", "d"};

    struct Relationship {
      std::string id;
      std::string type;
      std::string target;
    };

    struct SharedString {
      std::string text;
      bool clipped = false;
    };

    struct SharedStrings {
      std::vector<SharedString> kept;
      std::size_t total = 0; // strings in the file, kept or not
    };

    struct Workbook {
      std::vector<std::string> names;
      std::vector<std::string> relationshipIds;
      DateSystem system = DateSystem::Date1900;
    };

    struct CellReference {
      std::uint64_t column = 0;
      std::uint64_t row = 0;
    };

    // "B12" as column 1 and row 11; nothing past column XFD or row 1048576.
    std::optional<CellReference> parseReference(std::string_view text) {
      std::size_t letters = 0;
      std::uint64_t column = 0;
      while (letters < text.size() && letters <= kMaxColumnLetters && text[letters] >= 'A' && text[letters] <= 'Z') {
        column = (column * 26) + static_cast<std::uint64_t>(text[letters] - 'A' + 1);
        ++letters;
      }
      const auto row = parseSheetCount(text.substr(letters));
      if (letters == 0
          || letters > kMaxColumnLetters
          || column > kColumnCount
          || !row
          || *row == 0
          || *row > kRowCount) {
        return std::nullopt;
      }
      return CellReference{.column = column - 1, .row = *row - 1};
    }

    // Decodes "_xHHHH_" escapes of characters below U+0800; any other stays as written.
    std::string decodeEscapes(std::string_view text) {
      std::string out;
      for (std::size_t i = 0; i < text.size(); ++i) {
        const std::string_view rest = text.substr(i);
        std::uint32_t code = 0;
        const bool escape = rest.size() >= kEscapeBytes
            && rest.starts_with("_x")
            && rest[kEscapeBytes - 1] == '_'
            && std::ranges::all_of(rest.substr(2, 4), isAsciiHexDigit)
            && std::from_chars(rest.data() + 2, rest.data() + 6, code, 16).ec == std::errc{}
            && code < 0x800;
        if (escape) {
          appendUtf8(out, code);
        } else {
          out += text[i];
        }
        i += escape ? kEscapeBytes - 1 : 0;
      }
      return out;
    }

    // A workbook relationship target as a member name: from the archive root when it starts with '/', otherwise
    // from the workbook's folder.
    std::string resolveTarget(std::string_view target) {
      return target.starts_with('/') ? std::string(target.substr(1))
                                     : std::string(kWorkbookFolder) + std::string(target);
    }

    std::expected<std::vector<Relationship>, SheetError>
    parseRelationships(std::string_view bytes, std::string_view member) {
      std::vector<Relationship> relationships;
      auto visited = visitSheetXml(bytes, member, [&](const XmlEvent& event, std::size_t) -> SheetStep {
        if (event.kind != XmlEventKind::StartElement
            || !hasLocalName(event.name, "Relationship")
            || attributeValue(event.attributes, "TargetMode") == "External") {
          return true;
        }
        const auto id = attributeValue(event.attributes, "Id");
        const auto type = attributeValue(event.attributes, "Type");
        const auto target = attributeValue(event.attributes, "Target");
        if (!id || !type || !target) {
          return sheetFailure(SheetErrorKind::BadValue, member);
        }
        relationships.push_back({.id = std::string(*id), .type = std::string(*type), .target = std::string(*target)});
        return true;
      });
      return visited.transform([&] { return std::move(relationships); });
    }

    // The first relationship whose type ends with `kind`, such as "/styles".
    const Relationship* relationshipOfType(const std::vector<Relationship>& relationships, std::string_view kind) {
      const auto found = std::ranges::find_if(relationships, [kind](const Relationship& relationship) {
        return relationship.type.ends_with(kind);
      });
      return found == relationships.end() ? nullptr : &*found;
    }

    std::expected<Workbook, SheetError> parseWorkbook(std::string_view bytes, std::string_view member) {
      Workbook workbook;
      bool inSheets = false;
      auto visited = visitSheetXml(bytes, member, [&](const XmlEvent& event, std::size_t) -> SheetStep {
        const std::string_view local = localName(event.name);
        inSheets = local == "sheets" ? event.kind == XmlEventKind::StartElement : inSheets;
        if (event.kind == XmlEventKind::StartElement && local == "workbookPr") {
          const auto flag = attributeValue(event.attributes, "date1904").value_or("0");
          if (flag != "0" && flag != "1" && flag != "false" && flag != "true") {
            return sheetFailure(SheetErrorKind::BadValue, member);
          }
          workbook.system = flag == "1" || flag == "true" ? DateSystem::Date1904 : DateSystem::Date1900;
        } else if (event.kind == XmlEventKind::StartElement && local == "sheet" && inSheets) {
          const auto name = attributeValue(event.attributes, "name");
          const auto id = attributeValueByLocalName(event.attributes, "id");
          if (!name || !id) {
            return sheetFailure(SheetErrorKind::BadValue, member);
          }
          workbook.names.emplace_back(*name);
          workbook.relationshipIds.emplace_back(*id);
        }
        return true;
      });
      return visited.transform([&] { return std::move(workbook); });
    }

    // Text runs of each string item, phonetic runs left out.
    std::expected<SharedStrings, SheetError> parseSharedStrings(
        std::string_view bytes, std::string_view member, const SheetTableBuilder& builder, const SheetLimits& limits
    ) {
      SharedStrings strings;
      std::string current;
      bool inItem = false;
      bool inPhonetic = false;
      bool inText = false;
      auto visited = visitSheetXml(bytes, member, [&](const XmlEvent& event, std::size_t) -> SheetStep {
        const std::string_view local = localName(event.name);
        if (event.kind == XmlEventKind::StartElement) {
          if (local == "si") {
            current.clear();
            inItem = true;
          }
          inPhonetic = inPhonetic || (inItem && local == "rPh");
          inText = inText || (inItem && !inPhonetic && local == "t");
        } else if (event.kind == XmlEventKind::EndElement) {
          inPhonetic = inPhonetic && local != "rPh";
          inText = inText && local != "t";
          if (local == "si" && inItem) {
            inItem = false;
            if (strings.kept.size() < limits.maxSharedStrings) {
              SharedString kept{.text = decodeEscapes(current)};
              kept.clipped = builder.clip(kept.text);
              strings.kept.push_back(std::move(kept));
            }
            ++strings.total;
          }
        } else if (inText) {
          builder.collect(current, event.text);
        }
        return true;
      });
      return visited.transform([&] { return std::move(strings); });
    }

    // Reads the rows of one worksheet's sheetData into the builder.
    class SheetParser {
    public:
      SheetParser(
          std::string_view member, const SharedStrings& strings, const std::vector<XlsxFormat>& formats,
          DateSystem system, SheetTableBuilder& builder
      )
          : m_member(member), m_strings(strings), m_formats(formats), m_system(system), m_builder(builder) {}

      SheetStep visit(const XmlEvent& event) {
        const std::string_view local = localName(event.name);
        if (event.kind == XmlEventKind::StartElement) {
          return start(local, event.attributes).transform([] { return true; });
        }
        if (event.kind != XmlEventKind::EndElement) {
          if (m_capture) {
            m_builder.collect(m_value, event.text);
          }
          return true;
        }
        if (m_inCell) {
          m_capture = m_capture && local != "v" && local != "t";
          m_inInline = m_inInline && local != "is";
          m_inPhonetic = m_inPhonetic && local != "rPh";
          return local == "c" ? endCell().transform([] { return true; }) : SheetStep(true);
        }
        if (m_inRow && local == "row") {
          m_inRow = false;
          m_builder.addRow(m_row, std::move(m_cells));
          return !m_builder.full();
        }
        return !(m_inSheetData && local == "sheetData");
      }

    private:
      std::expected<void, SheetError> start(std::string_view local, std::span<const XmlAttribute> attributes) {
        if (m_inCell) {
          m_hasValue = m_hasValue || local == "v" || (local == "is" && m_type == CellType::InlineString);
          m_formula = m_formula || local == "f";
          m_inInline = m_inInline || local == "is";
          m_inPhonetic = m_inPhonetic || (m_inInline && local == "rPh");
          m_capture = !m_dropped && (m_capture || local == "v" || (m_inInline && !m_inPhonetic && local == "t"));
        } else if (m_inRow && local == "c") {
          return startCell(attributes);
        } else if (m_inSheetData && !m_inRow && local == "row") {
          const auto written = attributeValue(attributes, "r");
          const auto number = written ? parseSheetCount(*written) : std::optional(m_lastRow.value_or(0) + 1);
          const bool ascending = !m_lastRow || (number && *number > *m_lastRow);
          if (!number || *number == 0 || *number > kRowCount || !ascending) {
            return sheetFailure(SheetErrorKind::BadValue, m_member);
          }
          m_lastRow = number;
          m_row = *number - 1;
          m_lastColumn.reset();
          m_cells = SheetRow{};
          m_inRow = true;
        } else {
          m_inSheetData = m_inSheetData || local == "sheetData";
        }
        return {};
      }

      std::expected<void, SheetError> startCell(std::span<const XmlAttribute> attributes) {
        const auto reference = attributeValue(attributes, "r");
        const auto parsed = reference ? parseReference(*reference) : std::nullopt;
        const auto* const type = std::ranges::find(kCellTypeNames, attributeValue(attributes, "t").value_or("n"));
        const auto style = parseSheetCount(attributeValue(attributes, "s").value_or("0"));
        m_column = parsed ? parsed->column : (m_lastColumn ? *m_lastColumn + 1 : 0);
        if ((reference && (!parsed || parsed->row != m_row))
            || (m_lastColumn && m_column <= *m_lastColumn)
            || type == kCellTypeNames.end()
            || !style) {
          return sheetFailure(SheetErrorKind::BadValue, m_member);
        }
        m_lastColumn = m_column;
        m_dropped = !m_builder.columnFits(m_column);
        m_type = static_cast<CellType>(type - kCellTypeNames.begin());
        m_style = *style;
        m_value.clear();
        m_hasValue = false;
        m_formula = false;
        m_inInline = false;
        m_inPhonetic = false;
        m_capture = false;
        m_inCell = true;
        return {};
      }

      std::expected<void, SheetError> endCell() {
        m_inCell = false;
        if (m_hasValue && m_dropped) {
          m_builder.markTruncated();
        }
        if (!m_hasValue || m_dropped) {
          return {};
        }
        SheetCell cell{.text = {}, .formula = m_formula};
        std::optional<double> number;
        switch (m_type) {
        case CellType::SharedString: {
          const auto index = parseSheetCount(m_value);
          if (index && *index >= m_strings.total) {
            return sheetFailure(SheetErrorKind::SharedStringIndex, m_member);
          }
          if (index && *index >= m_strings.kept.size()) {
            m_builder.markTruncated();
            return {};
          }
          if (index && m_strings.kept[*index].clipped) {
            m_builder.markTruncated();
          }
          cell.text = index ? m_strings.kept[*index].text : std::string();
          number = index ? std::optional(0.0) : std::nullopt;
          break;
        }
        case CellType::InlineString:
        case CellType::FormulaString:
          cell.text = decodeEscapes(m_value);
          number = 0.0;
          break;
        case CellType::Error:
          cell.kind = SheetCellKind::Error;
          cell.text = m_value;
          number = 0.0;
          break;
        case CellType::Boolean:
          cell.kind = SheetCellKind::Boolean;
          number = m_value == "0" || m_value == "1" ? std::optional(m_value == "1" ? 1.0 : 0.0) : std::nullopt;
          break;
        case CellType::Date:
          number = sheetTimeFromIso(m_value);
          cell.kind = number ? SheetCellKind::Time : SheetCellKind::Date;
          number = number ? number : sheetSerialFromIso(m_value, m_system);
          break;
        case CellType::Number: {
          const XlsxFormat format = m_style < m_formats.size() ? m_formats[m_style] : XlsxFormat::Number;
          cell.kind = format == XlsxFormat::Date
              ? SheetCellKind::Date
              : (format == XlsxFormat::Number ? SheetCellKind::Number : SheetCellKind::Time);
          number = parseSheetNumber(m_value);
          if (number && format == XlsxFormat::Clock) {
            number = *number - std::floor(*number);
          }
          break;
        }
        }
        if (!number) {
          return sheetFailure(SheetErrorKind::BadValue, m_member);
        }
        cell.number = *number;
        m_builder.addCell(m_cells, m_column, std::move(cell));
        return {};
      }

      std::string_view m_member;
      const SharedStrings& m_strings;
      const std::vector<XlsxFormat>& m_formats;
      DateSystem m_system;
      SheetTableBuilder& m_builder;
      SheetRow m_cells;
      std::string m_value;
      std::optional<std::uint64_t> m_lastRow;    // counted from 1
      std::optional<std::uint64_t> m_lastColumn; // counted from 0
      std::uint64_t m_row = 0;
      std::uint64_t m_column = 0;
      std::uint64_t m_style = 0;
      CellType m_type = CellType::Number;
      bool m_inSheetData = false;
      bool m_inRow = false;
      bool m_inCell = false;
      bool m_inInline = false;
      bool m_inPhonetic = false;
      bool m_capture = false;
      bool m_hasValue = false;
      bool m_formula = false;
      bool m_dropped = false; // the cell is past the column limit, so its value is not read
    };

    template <typename Parse> auto readPart(const MemberReader& members, std::string_view member, Parse&& parse) {
      return readSheetMember(members, member).and_then([&](const std::string& bytes) { return parse(bytes, member); });
    }
  } // namespace

  std::expected<SheetBook, SheetError>
  readXlsx(const MemberReader& members, std::size_t sheetIndex, const SheetLimits& limits) {
    const auto workbook = readPart(members, kWorkbook, parseWorkbook);
    if (!workbook) {
      return std::unexpected(workbook.error());
    }
    const auto parts = readPart(members, kWorkbookRelationships, parseRelationships);
    if (!parts) {
      return std::unexpected(parts.error());
    }
    if (sheetIndex >= workbook->names.size()) {
      return sheetFailure(SheetErrorKind::SheetIndex, kWorkbook);
    }
    const auto sheet = std::ranges::find(*parts, workbook->relationshipIds[sheetIndex], &Relationship::id);
    if (sheet == parts->end()) {
      return sheetFailure(SheetErrorKind::MissingRelationship, kWorkbookRelationships);
    }

    SheetTableBuilder builder(limits);
    const Relationship* styles = relationshipOfType(*parts, "/styles");
    const auto formats = styles == nullptr ? std::expected<std::vector<XlsxFormat>, SheetError>{}
                                           : readPart(members, resolveTarget(styles->target), parseXlsxStyles);
    if (!formats) {
      return std::unexpected(formats.error());
    }
    const Relationship* shared = relationshipOfType(*parts, "/sharedStrings");
    const auto strings = shared == nullptr
        ? std::expected<SharedStrings, SheetError>{}
        : readPart(members, resolveTarget(shared->target), [&](std::string_view bytes, std::string_view member) {
            return parseSharedStrings(bytes, member, builder, limits);
          });
    if (!strings) {
      return std::unexpected(strings.error());
    }
    const std::string sheetPath = resolveTarget(sheet->target);
    SheetParser parser(sheetPath, *strings, *formats, workbook->system, builder);
    const auto read = readPart(members, sheetPath, [&](std::string_view bytes, std::string_view member) {
      return visitSheetXml(bytes, member, [&](const XmlEvent& event, std::size_t) { return parser.visit(event); });
    });
    if (!read) {
      return std::unexpected(read.error());
    }
    return SheetBook{
        .sheetNames = workbook->names, .sheetIndex = sheetIndex, .table = builder.finish(workbook->system)
    };
  }
} // namespace luciole::formats
