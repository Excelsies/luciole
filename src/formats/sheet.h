#pragma once

#include "formats/xml_reader.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luciole::formats {
  enum class SheetCellKind : std::uint8_t {
    Text,
    Number,
    Boolean,
    Date,  // `number` is a serial day in the table's date system, with the time of day as its fraction
    Time,  // `number` is a duration in days; a time of day is the duration since midnight
    Error, // `text` is the error value, such as #DIV/0!
  };

  enum class DateSystem : std::uint8_t {
    Date1900, // serial 1 is 1900-01-01, and serial 60 is the 1900-02-29 that never was
    Date1904, // serial 0 is 1904-01-01
  };

  // A row at or past maxRows ends the table; a cell at or past maxColumns is left out; storing stops at maxCells
  // cells or maxTextBytes bytes of cell text; text is cut to maxCellChars code points. Each of these sets
  // SheetTable::truncated. XLSX keeps at most maxSharedStrings shared strings; a cell naming a later one is left
  // out and sets truncated.
  struct SheetLimits {
    std::size_t maxRows = 5'000;
    std::size_t maxColumns = 256;
    std::size_t maxCells = 200'000;
    std::size_t maxCellChars = 32'768;
    std::size_t maxTextBytes = 16 * 1024 * 1024;
    std::size_t maxSharedStrings = 200'000;
  };

  struct SheetCell {
    std::uint32_t column = 0;
    SheetCellKind kind = SheetCellKind::Text;
    double number = 0.0;  // Number, Date and Time; Boolean is 0 or 1
    std::string text;     // Text and Error
    bool formula = false; // the value is the cached result of a formula
    bool operator==(const SheetCell&) const = default;
  };

  struct SheetRow {
    std::uint32_t index = 0;
    std::vector<SheetCell> cells; // ascending columns, empty cells left out
  };

  // Inclusive bounds of the stored cells.
  struct SheetRange {
    std::uint32_t firstRow = 0;
    std::uint32_t firstColumn = 0;
    std::uint32_t lastRow = 0;
    std::uint32_t lastColumn = 0;
    bool operator==(const SheetRange&) const = default;
  };

  struct SheetTable {
    std::vector<SheetRow> rows; // ascending indices, rows without cells left out
    std::optional<SheetRange> usedRange = std::nullopt;
    DateSystem dateSystem = DateSystem::Date1900;
    bool truncated = false; // a limit left something out
  };

  struct SheetBook {
    std::vector<std::string> sheetNames; // workbook order
    std::size_t sheetIndex = 0;          // the sheet in `table`
    SheetTable table;
  };

  enum class SheetErrorKind : std::uint8_t {
    MissingMember,       // the archive has no member by a name the file needs
    Xml,                 // a member is not well-formed XML or passes an XML limit
    SheetIndex,          // the requested sheet does not exist
    MissingRelationship, // the workbook names a relationship the package does not have
    SharedStringIndex,   // a cell names a shared string the file does not have
    BadValue,            // an attribute, reference, index or value that cannot be read
  };

  struct SheetError {
    SheetErrorKind kind = SheetErrorKind::BadValue;
    std::string member;                         // the archive member it concerns
    std::optional<XmlError> xml = std::nullopt; // for SheetErrorKind::Xml
  };

  // The bytes of the archive member with this name, or nothing when there is none.
  using MemberReader = std::function<std::optional<std::string>(std::string_view name)>;

  namespace sheet_detail {
    inline constexpr std::int64_t kSecondsPerDay = 86'400;
    // Serials beyond this many days are not formatted, which keeps the second count in range.
    inline constexpr double kMaxSerialDays = 1.0E7;

    // Days from 1970-01-01 to a proleptic Gregorian date.
    constexpr std::int64_t daysFromCivil(std::int64_t year, std::int64_t month, std::int64_t day) {
      year -= month <= 2 ? 1 : 0;
      const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
      const std::int64_t yearOfEra = year - (era * 400);
      const std::int64_t dayOfYear = (((153 * (month + (month > 2 ? -3 : 9))) + 2) / 5) + day - 1;
      const std::int64_t dayOfEra = (yearOfEra * 365) + (yearOfEra / 4) - (yearOfEra / 100) + dayOfYear;
      return (era * 146'097) + dayOfEra - 719'468;
    }

    struct CivilDate {
      std::int64_t year = 0;
      std::int64_t month = 0;
      std::int64_t day = 0;
    };

    constexpr CivilDate civilFromDays(std::int64_t days) {
      days += 719'468;
      const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
      const std::int64_t dayOfEra = days - (era * 146'097);
      const std::int64_t yearOfEra = (dayOfEra - (dayOfEra / 1'460) + (dayOfEra / 36'524) - (dayOfEra / 146'096)) / 365;
      const std::int64_t dayOfYear = dayOfEra - ((365 * yearOfEra) + (yearOfEra / 4) - (yearOfEra / 100));
      const std::int64_t monthIndex = ((5 * dayOfYear) + 2) / 153;
      const std::int64_t day = dayOfYear - (((153 * monthIndex) + 2) / 5) + 1;
      const std::int64_t month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
      return {.year = (yearOfEra + (era * 400)) + (month <= 2 ? 1 : 0), .month = month, .day = day};
    }

    // The serial of a day counted from 1970-01-01, and back.
    inline std::int64_t serialFromDays(std::int64_t days, DateSystem system) {
      if (system == DateSystem::Date1904) {
        return days - daysFromCivil(1904, 1, 1);
      }
      const std::int64_t serial = days - daysFromCivil(1899, 12, 30);
      return serial < 61 ? serial - 1 : serial;
    }

    // A non-negative integer of exactly `count` ASCII digits.
    inline std::optional<std::int64_t> digits(std::string_view text, std::size_t at, std::size_t count) {
      if (at + count > text.size()) {
        return std::nullopt;
      }
      std::int64_t value = 0;
      const char* first = text.data() + at;
      const auto [end, status] = std::from_chars(first, first + count, value);
      if (status != std::errc{} || end != first + count || !std::ranges::all_of(text.substr(at, count), [](char c) {
            return c >= '0' && c <= '9';
          })) {
        return std::nullopt;
      }
      return value;
    }

    // Days in a month from 1 to 12.
    inline std::int64_t daysInMonth(std::int64_t year, std::int64_t month) {
      constexpr std::array<std::int64_t, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
      return month == 2 && leap ? 29 : kDays.at(static_cast<std::size_t>(month - 1));
    }

    // The text without a trailing time-zone designator: "Z", or "+HH:MM" or "-HH:MM" with hours below 24.
    inline std::string_view withoutZone(std::string_view text) {
      if (text.ends_with('Z')) {
        return text.substr(0, text.size() - 1);
      }
      constexpr std::size_t kOffsetBytes = 6;
      const std::size_t at = text.size() < kOffsetBytes ? 0 : text.size() - kOffsetBytes;
      const auto hours = digits(text, at + 1, 2);
      const auto minutes = digits(text, at + 4, 2);
      const bool offset = text.size() >= kOffsetBytes
          && (text[at] == '+' || text[at] == '-')
          && text[at + 3] == ':'
          && hours
          && minutes
          && *hours < 24
          && *minutes < 60;
      return offset ? text.substr(0, at) : text;
    }

    // Seconds of "HH:MM:SS" with an optional fraction of a second, read in full.
    inline std::optional<double> secondsOfDay(std::string_view text) {
      const auto hour = digits(text, 0, 2);
      const auto minute = digits(text, 3, 2);
      const auto second = digits(text, 6, 2);
      if (!hour
          || !minute
          || !second
          || text[2] != ':'
          || text[5] != ':'
          || *hour > 23
          || *minute > 59
          || *second > 59) {
        return std::nullopt;
      }
      auto seconds = static_cast<double>((*hour * 3600) + (*minute * 60) + *second);
      const std::string_view fraction = text.substr(8);
      if (!fraction.empty()) {
        double part = 0.0;
        const auto [end, status] = std::from_chars(fraction.data(), fraction.data() + fraction.size(), part);
        if (fraction.size() < 2
            || fraction[0] != '.'
            || status != std::errc{}
            || end != fraction.data() + fraction.size()
            || !std::ranges::all_of(fraction.substr(1), [](char c) { return c >= '0' && c <= '9'; })) {
          return std::nullopt;
        }
        seconds += part;
      }
      return seconds;
    }
  } // namespace sheet_detail

  // A serial as an ISO date ("2023-03-15"), or as date and time ("2023-03-15T12:00:00") when the time of day
  // rounds to a whole second other than midnight. Serial 60 of the 1900 system is "1900-02-29". Nothing for a
  // value that is not finite or falls outside the years 1 to 9999.
  inline std::optional<std::string> formatSheetDate(double serial, DateSystem system) {
    using namespace sheet_detail;
    if (!std::isfinite(serial) || std::abs(serial) > kMaxSerialDays) {
      return std::nullopt;
    }
    const auto seconds = static_cast<std::int64_t>(std::llround(serial * static_cast<double>(kSecondsPerDay)));
    const std::int64_t day = (seconds >= 0 ? seconds : seconds - (kSecondsPerDay - 1)) / kSecondsPerDay;
    const std::int64_t secondOfDay = seconds - (day * kSecondsPerDay);
    CivilDate date{.year = 1900, .month = 2, .day = 29};
    if (system == DateSystem::Date1904) {
      date = civilFromDays(daysFromCivil(1904, 1, 1) + day);
    } else if (day != 60) {
      date = civilFromDays(daysFromCivil(1899, 12, day < 60 ? 31 : 30) + day);
    }
    if (date.year < 1 || date.year > 9999) {
      return std::nullopt;
    }
    std::string out = std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
    if (secondOfDay != 0) {
      out += std::format("T{:02}:{:02}:{:02}", secondOfDay / 3600, (secondOfDay / 60) % 60, secondOfDay % 60);
    }
    return out;
  }

  // A duration in days as hours, minutes and seconds ("36:00:00"), rounded to the second. Nothing for a value
  // that is negative or not finite.
  inline std::optional<std::string> formatSheetTime(double days) {
    using namespace sheet_detail;
    if (!std::isfinite(days) || days < 0.0 || days > kMaxSerialDays) {
      return std::nullopt;
    }
    const auto seconds = static_cast<std::int64_t>(std::llround(days * static_cast<double>(kSecondsPerDay)));
    return std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
  }

  // The serial of "YYYY-MM-DD" with an optional "THH:MM:SS" and fraction of a second, then an optional time-zone
  // designator that is passed over; nothing for any other text or a date or time that does not exist.
  inline std::optional<double> sheetSerialFromIso(std::string_view text, DateSystem system) {
    using namespace sheet_detail;
    text = withoutZone(text);
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    if (!year
        || !month
        || !day
        || text[4] != '-'
        || text[7] != '-'
        || *month < 1
        || *month > 12
        || *day < 1
        || *day > daysInMonth(*year, *month)) {
      return std::nullopt;
    }
    std::optional<double> seconds = 0.0;
    if (text.size() > 10) {
      seconds = text[10] == 'T' ? secondsOfDay(text.substr(11)) : std::nullopt;
    }
    if (!seconds) {
      return std::nullopt;
    }
    const std::int64_t serial = serialFromDays(daysFromCivil(*year, *month, *day), system);
    return static_cast<double>(serial) + (*seconds / static_cast<double>(kSecondsPerDay));
  }

  // The time of day of "HH:MM:SS" with an optional fraction of a second, then an optional time-zone designator
  // that is passed over, in days; nothing for any other text or a time that does not exist.
  inline std::optional<double> sheetTimeFromIso(std::string_view text) {
    using namespace sheet_detail;
    return secondsOfDay(withoutZone(text)).transform([](double seconds) {
      return seconds / static_cast<double>(kSecondsPerDay);
    });
  }

  // A finite decimal number with optional surrounding ASCII whitespace, read in full.
  inline std::optional<double> parseSheetNumber(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && blank(text.front())) {
      text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
      text.remove_suffix(1);
    }
    double value = 0.0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || status != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  // A non-negative decimal integer of ASCII digits only, read in full.
  inline std::optional<std::uint64_t> parseSheetCount(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || status != std::errc{} || end != text.data() + text.size()) {
      return std::nullopt;
    }
    return value;
  }

  inline std::unexpected<SheetError> sheetFailure(SheetErrorKind kind, std::string_view member) {
    return std::unexpected(SheetError{.kind = kind, .member = std::string(member)});
  }

  // What a visitor of XML events returns: true to go on, false to stop, or an error.
  using SheetStep = std::expected<bool, SheetError>;

  // The bytes of a member, or a MissingMember error naming it.
  inline std::expected<std::string, SheetError> readSheetMember(const MemberReader& members, std::string_view name) {
    std::optional<std::string> bytes = members(name);
    if (!bytes) {
      return sheetFailure(SheetErrorKind::MissingMember, name);
    }
    return std::move(*bytes);
  }

  // Calls `visit(event, depth)` for each event of a member until End, an error, or a visit that returns false.
  // The reader's event limit bounds the loop.
  template <typename Visit>
  std::expected<void, SheetError> visitSheetXml(std::string_view bytes, std::string_view member, Visit&& visit) {
    const XmlLimits limits;
    XmlReader reader(bytes, limits);
    for (std::size_t calls = 0; calls <= limits.maxEvents; ++calls) {
      const auto event = reader.next();
      if (!event) {
        return std::unexpected(
            SheetError{.kind = SheetErrorKind::Xml, .member = std::string(member), .xml = event.error()}
        );
      }
      if (event->kind == XmlEventKind::End) {
        return {};
      }
      const SheetStep more = visit(*event, reader.depth());
      if (!more) {
        return std::unexpected(more.error());
      }
      if (!*more) {
        return {};
      }
    }
    return std::unexpected(
        SheetError{.kind = SheetErrorKind::Xml, .member = std::string(member), .xml = XmlError::TooManyEvents}
    );
  }

  // Collects rows under SheetLimits. Rows are added in ascending index order.
  class SheetTableBuilder {
  public:
    explicit SheetTableBuilder(const SheetLimits& limits) : m_limits(limits) {}

    // A limit ended the table; later rows are not stored.
    bool full() const { return m_full; }

    void markTruncated() { m_table.truncated = true; }

    // Appends text to a cell's text while it holds fewer bytes than the most maxCellChars code points can take,
    // plus one, so clip still sees that it is too long.
    void collect(std::string& out, std::string_view text) const {
      const std::size_t cap = m_limits.maxCellChars > (std::numeric_limits<std::size_t>::max() - 5) / 4
          ? std::numeric_limits<std::size_t>::max()
          : (m_limits.maxCellChars * 4) + 4;
      out.append(text.substr(0, cap - std::min(cap, out.size())));
    }

    // Cuts text to maxCellChars code points; true when it did.
    bool clip(std::string& text) const {
      std::size_t count = 0;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U && count++ == m_limits.maxCellChars) {
          text.resize(i);
          return true;
        }
      }
      return false;
    }

    // Whether a cell at `column` can be stored: the column is below maxColumns.
    bool columnFits(std::uint64_t column) const {
      return column < std::min<std::uint64_t>(m_limits.maxColumns, std::numeric_limits<std::uint32_t>::max());
    }

    // Adds a cell to a row being built. A column at or past maxColumns is left out, and text is clipped; either
    // marks the table truncated.
    void addCell(SheetRow& row, std::uint64_t column, SheetCell cell) {
      if (!columnFits(column)) {
        markTruncated();
        return;
      }
      cell.column = static_cast<std::uint32_t>(column);
      if (clip(cell.text)) {
        markTruncated();
      }
      row.cells.push_back(std::move(cell));
    }

    // Stores a row that has cells at `index`. A row at or past maxRows ends the table when it has cells or the
    // table is already truncated, since every later row is past the limit too. A row that passes the cell or text
    // limit ends the table, and the cells that fit are kept.
    void addRow(std::uint64_t index, SheetRow row) {
      if (m_full) {
        return;
      }
      if (index >= std::min<std::uint64_t>(m_limits.maxRows, std::numeric_limits<std::uint32_t>::max())) {
        if (!row.cells.empty() || m_table.truncated) {
          end();
        }
        return;
      }
      if (row.cells.empty()) {
        return;
      }
      std::size_t fit = 0;
      while (fit < row.cells.size()
             && m_cells < m_limits.maxCells
             && row.cells[fit].text.size() <= m_limits.maxTextBytes - m_textBytes) {
        m_textBytes += row.cells[fit].text.size();
        ++m_cells;
        ++fit;
      }
      if (fit < row.cells.size()) {
        row.cells.resize(fit);
        end();
      }
      if (fit > 0) {
        row.index = static_cast<std::uint32_t>(index);
        m_table.rows.push_back(std::move(row));
      }
    }

    SheetTable finish(DateSystem system) {
      m_table.dateSystem = system;
      if (!m_table.rows.empty()) {
        SheetRange range{
            .firstRow = m_table.rows.front().index,
            .firstColumn = std::numeric_limits<std::uint32_t>::max(),
            .lastRow = m_table.rows.back().index,
            .lastColumn = 0,
        };
        for (const SheetRow& row : m_table.rows) {
          range.firstColumn = std::min(range.firstColumn, row.cells.front().column);
          range.lastColumn = std::max(range.lastColumn, row.cells.back().column);
        }
        m_table.usedRange = range;
      }
      return std::move(m_table);
    }

  private:
    void end() {
      m_full = true;
      markTruncated();
    }

    SheetLimits m_limits;
    SheetTable m_table;
    std::size_t m_cells = 0;
    std::size_t m_textBytes = 0;
    bool m_full = false;
  };
} // namespace luciole::formats
