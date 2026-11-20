#include "formats/sheet_xlsx_styles.h"

#include "formats/ascii.h"
#include "formats/xml_reader.h"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

namespace luciole::formats {
  namespace {
    // Cell formats kept from the styles; cells using a later one show plain numbers.
    constexpr std::size_t kMaxCellFormats = 65'536;
    // Bytes of a format code scanned for date and time letters.
    constexpr std::size_t kMaxFormatScan = 256;

    // Bracketed format text that shows elapsed time: one letter of h, m or s, repeated, in either case.
    bool isElapsedToken(std::string_view token) {
      const char first = token.empty() ? '\0' : toAsciiLower(token.front());
      return (first == 'h' || first == 'm' || first == 's')
          && std::ranges::all_of(token, [first](char c) { return toAsciiLower(c) == first; });
    }

    // Quoted and other bracketed text and escaped characters are passed over.
    XlsxFormat scanFormatCode(std::string_view code) {
      bool date = false;
      bool month = false;
      bool time = false;
      bool elapsed = false;
      const std::size_t end = std::min(code.size(), kMaxFormatScan);
      for (std::size_t i = 0; i < end; ++i) {
        const char c = code[i];
        if (c == '"' || c == '[') {
          const std::size_t close = code.find(c == '"' ? '"' : ']', i + 1);
          elapsed = elapsed
              || (c == '[' && close != std::string_view::npos && isElapsedToken(code.substr(i + 1, close - i - 1)));
          i = close == std::string_view::npos ? end : close;
        } else if (c == '\\' || c == '_' || c == '*') {
          ++i;
        } else {
          const char lower = toAsciiLower(c);
          date = date || lower == 'y' || lower == 'd';
          month = month || lower == 'm';
          time = time || lower == 'h' || lower == 's';
        }
      }
      if (date || (month && !time && !elapsed)) {
        return XlsxFormat::Date;
      }
      return elapsed ? XlsxFormat::Elapsed : (time ? XlsxFormat::Clock : XlsxFormat::Number);
    }

    XlsxFormat builtinFormat(std::uint64_t id) {
      if ((id >= 14 && id <= 17) || id == 22) {
        return XlsxFormat::Date;
      }
      return (id >= 18 && id <= 21) || id == 45 || id == 47 ? XlsxFormat::Clock
                                                            : (id == 46 ? XlsxFormat::Elapsed : XlsxFormat::Number);
    }
  } // namespace

  std::expected<std::vector<XlsxFormat>, SheetError> parseXlsxStyles(std::string_view bytes, std::string_view member) {
    std::map<std::uint64_t, XlsxFormat> custom;
    std::vector<std::uint64_t> ids;
    bool inNumberFormats = false;
    bool inCellFormats = false;
    auto visited = visitSheetXml(bytes, member, [&](const XmlEvent& event, std::size_t) -> SheetStep {
      const std::string_view local = localName(event.name);
      const bool start = event.kind == XmlEventKind::StartElement;
      inNumberFormats = local == "numFmts" ? start : inNumberFormats;
      inCellFormats = local == "cellXfs" ? start : inCellFormats;
      std::optional<std::uint64_t> id;
      if (start && local == "numFmt" && inNumberFormats) {
        id = parseSheetCount(attributeValue(event.attributes, "numFmtId").value_or(""));
        if (id) {
          custom[*id] = scanFormatCode(attributeValue(event.attributes, "formatCode").value_or(""));
        }
      } else if (start && local == "xf" && inCellFormats) {
        id = parseSheetCount(attributeValue(event.attributes, "numFmtId").value_or("0"));
        if (id && ids.size() < kMaxCellFormats) {
          ids.push_back(*id);
        }
      } else {
        return true;
      }
      return id ? SheetStep(true) : sheetFailure(SheetErrorKind::BadValue, member);
    });
    std::vector<XlsxFormat> kinds;
    kinds.reserve(ids.size());
    for (const std::uint64_t id : ids) {
      const auto found = custom.find(id);
      kinds.push_back(found != custom.end() ? found->second : builtinFormat(id));
    }
    return visited.transform([&] { return std::move(kinds); });
  }
} // namespace luciole::formats
