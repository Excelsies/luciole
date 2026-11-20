#include "formats/sheet.h"
#include "formats/sheet_xlsx.h"
#include "test_check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
  // Allocations of at least kLargeAllocation bytes since the last reset, counted by the operator new below.
  constexpr std::size_t kLargeAllocation = 16 * 1024;
  std::size_t largeAllocations = 0;
} // namespace

// The replacements stay out of line so the compiler does not pair a free with a new it can see.
[[gnu::noinline]] void* operator new(std::size_t size) {
  largeAllocations += size >= kLargeAllocation ? 1 : 0;
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }

[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
  using namespace luciole::formats;

  using Members = std::map<std::string, std::string, std::less<>>;

  constexpr std::string_view kSheet1 = "xl/worksheets/sheet1.xml";
  constexpr std::string_view kSheet2 = "xl/worksheets/sheet2.xml";
  constexpr std::string_view kShared = "xl/sharedStrings.xml";

  MemberReader readerOf(const Members& members) {
    return [&members](std::string_view name) -> std::optional<std::string> {
      const auto found = members.find(name);
      if (found == members.end()) {
        return std::nullopt;
      }
      return found->second;
    };
  }

  std::string relationship(std::string_view id, std::string_view type, std::string_view target) {
    return std::format(
        R"(<Relationship Id="{}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/{}" )"
        R"(Target="{}"/>)",
        id, type, target
    );
  }

  std::string worksheet(std::string_view rows) {
    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        "\n"
        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<dimension ref="A1:IW7"/><sheetData>{}</sheetData><pageMargins left="0.7"/></worksheet>)",
        rows
    );
  }

  // A workbook with two sheets, shared strings and styles.
  Members workbook(std::string_view date1904 = "0") {
    Members members;
    members["xl/workbook.xml"] = std::format(
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )"
        R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
        R"(<workbookPr date1904="{}"/><sheets><sheet name="Data &amp; more" sheetId="1" r:id="rId1"/>)"
        R"(<sheet name="Second" sheetId="2" r:id="rId2"/></sheets></workbook>)",
        date1904
    );
    members["xl/_rels/workbook.xml.rels"] = "<Relationships>"
        + relationship("rId1", "worksheet", "worksheets/sheet1.xml")
        + relationship("rId2", "worksheet", "/xl/worksheets/sheet2.xml")
        + relationship("rId3", "sharedStrings", "sharedStrings.xml")
        + relationship("rId4", "styles", "styles.xml")
        + R"(<Relationship Id="rId5" Type="hyperlink" Target="http://example.com" TargetMode="External"/>)"
          "</Relationships>";
    members[std::string(kShared)] = R"(<sst count="5" uniqueCount="5"><si><t>Name</t></si>)"
                                    R"(<si><r><rPr><b/></rPr><t>Bold</t></r><r><t xml:space="preserve"> text</t></r>)"
                                    R"(<rPh sb="0" eb="1"><t>reading</t></rPh><phoneticPr fontId="1"/></si>)"
                                    R"(<si><t>a_x000D_b _x005F_x0041_ &amp; &lt; _x00e9_</t></si><si/>)"
                                    "<si><t>\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80x</t></si></sst>";
    members["xl/styles.xml"] =
        R"(<styleSheet><numFmts count="3"><numFmt numFmtId="164" formatCode="yyyy\-mm\-dd hh:mm"/>)"
        R"(<numFmt numFmtId="165" formatCode="&quot;days&quot; 0"/><numFmt numFmtId="166" formatCode="[Red]0.00"/>)"
        R"(</numFmts><cellStyleXfs count="1"><xf numFmtId="14"/></cellStyleXfs>)"
        R"(<cellXfs count="8"><xf numFmtId="0"/><xf numFmtId="14"/><xf numFmtId="164"><alignment/></xf>)"
        R"(<xf numFmtId="165"/><xf numFmtId="166"/><xf numFmtId="21"/><xf numFmtId="47"/><xf numFmtId="167"/>)"
        R"(</cellXfs><dxfs><dxf><numFmt numFmtId="167" formatCode="yyyy"/></dxf></dxfs></styleSheet>)";
    members[std::string(kSheet1)] = worksheet(
        R"(<row r="1" spans="1:3"><c r="A1" t="s"><v>0</v></c>)"
        R"(<c r="B1" t="inlineStr"><is><r><t>In</t></r><r><t>line</t></r><rPh><t>x</t></rPh></is></c>)"
        R"(<c r="C1" t="str"><f>A1&amp;B1</f><v>NameInline</v></c></row>)"
        R"(<row r="3"><c r="A3"><v>42.5</v></c><c r="B3" t="b"><v>1</v></c><c r="C3" s="1"><v>45000</v></c>)"
        R"(<c r="D3" s="2"><v>45000.5</v></c><c r="E3" t="e"><f>1/0</f><v>#DIV/0!</v></c>)"
        R"(<c r="F3"><f>3+4</f><v>7</v></c><c r="G3" s="3"><v>5</v></c><c r="H3" s="4"><v>-1.25</v></c>)"
        R"(<c r="I3" s="5"><v>0.75</v></c></row>)"
        R"(<row r="4"><c r="B4" t="s"><v>1</v></c><c t="s"><v>2</v></c><c r="E4" t="s"><v>3</v></c>)"
        R"(<c r="F4" s="6"><v>0.5</v></c><c r="IV4" t="b"><v>0</v></c><c r="IW4"><v>9</v></c></row>)"
        R"(<row r="5"><c r="A5" s="7"><v>3</v></c><c r="B5"/><c r="C5" s="1"/>)"
        R"(<c r="D5" t="d"><v>2023-03-15T06:00:00</v></c><c r="E5" t="inlineStr"/><c r="F5" t="s"><f>X</f></c></row>)"
        R"(<row r="6"><c r="A6" s="2"/></row><row><c t="b"><v>0</v></c></row>)"
    );
    members[std::string(kSheet2)] =
        R"(<x:worksheet xmlns:x="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<x:sheetData><x:row r="2"><x:c r="B2"><x:v>1</x:v></x:c></x:row></x:sheetData></x:worksheet>)";
    return members;
  }

  const SheetCell* cellAt(const SheetTable& table, std::uint32_t row, std::uint32_t column) {
    for (const SheetRow& stored : table.rows) {
      if (stored.index != row) {
        continue;
      }
      for (const SheetCell& cell : stored.cells) {
        if (cell.column == column) {
          return &cell;
        }
      }
    }
    return nullptr;
  }

  bool hasText(const SheetTable& table, std::uint32_t row, std::uint32_t column, std::string_view text) {
    const SheetCell* cell = cellAt(table, row, column);
    return cell != nullptr && cell->kind == SheetCellKind::Text && cell->text == text;
  }

  bool hasNumber(const SheetTable& table, std::uint32_t row, std::uint32_t column, SheetCellKind kind, double value) {
    const SheetCell* cell = cellAt(table, row, column);
    return cell != nullptr && cell->kind == kind && std::abs(cell->number - value) < 1.0E-9;
  }

  std::vector<std::uint32_t> rowIndices(const SheetTable& table) {
    std::vector<std::uint32_t> indices;
    indices.reserve(table.rows.size());
    for (const SheetRow& row : table.rows) {
      indices.push_back(row.index);
    }
    return indices;
  }

  std::expected<SheetBook, SheetError>
  readWith(const Members& members, std::size_t sheet = 0, const SheetLimits& limits = {}) {
    return readXlsx(readerOf(members), sheet, limits);
  }

  bool failsWith(
      const std::expected<SheetBook, SheetError>& result, SheetErrorKind kind, std::string_view member,
      std::optional<XmlError> xml = std::nullopt
  ) {
    return !result && result.error().kind == kind && result.error().member == member && result.error().xml == xml;
  }

  void testWorkbookSheetsAndCells() {
    const Members members = workbook();
    const auto book = readWith(members);
    TEST_CHECK(book.has_value());
    TEST_CHECK((book->sheetNames == std::vector<std::string>{"Data & more", "Second"}));
    TEST_CHECK(book->sheetIndex == 0);
    const SheetTable& table = book->table;
    TEST_CHECK(table.dateSystem == DateSystem::Date1900);
    TEST_CHECK((rowIndices(table) == std::vector<std::uint32_t>{0, 2, 3, 4, 6}));
    // Shared, rich and inline strings, and a formula string with its cached value.
    TEST_CHECK(hasText(table, 0, 0, "Name"));
    TEST_CHECK(hasText(table, 0, 1, "Inline"));
    TEST_CHECK(hasText(table, 0, 2, "NameInline"));
    TEST_CHECK(cellAt(table, 0, 2)->formula);
    TEST_CHECK(!cellAt(table, 0, 0)->formula);
    // Numbers, booleans, dates and times by style, errors, and formulas.
    TEST_CHECK(hasNumber(table, 2, 0, SheetCellKind::Number, 42.5));
    TEST_CHECK(hasNumber(table, 2, 1, SheetCellKind::Boolean, 1.0));
    TEST_CHECK(hasNumber(table, 2, 2, SheetCellKind::Date, 45000.0));
    TEST_CHECK(hasNumber(table, 2, 3, SheetCellKind::Date, 45000.5));
    const SheetCell* error = cellAt(table, 2, 4);
    TEST_CHECK(error != nullptr && error->kind == SheetCellKind::Error && error->text == "#DIV/0!" && error->formula);
    TEST_CHECK(hasNumber(table, 2, 5, SheetCellKind::Number, 7.0));
    TEST_CHECK(cellAt(table, 2, 5)->formula);
    TEST_CHECK(hasNumber(table, 2, 6, SheetCellKind::Number, 5.0));
    TEST_CHECK(hasNumber(table, 2, 7, SheetCellKind::Number, -1.25));
    TEST_CHECK(hasNumber(table, 2, 8, SheetCellKind::Time, 0.75));
    TEST_CHECK(formatSheetDate(cellAt(table, 2, 2)->number, table.dateSystem) == "2023-03-15");
    TEST_CHECK(formatSheetDate(cellAt(table, 2, 3)->number, table.dateSystem) == "2023-03-15T12:00:00");
    TEST_CHECK(formatSheetTime(cellAt(table, 2, 8)->number) == "18:00:00");
  }

  void testSparseCellsAndReferences() {
    const auto book = readWith(workbook());
    TEST_CHECK(book.has_value());
    const SheetTable& table = book->table;
    // A cell without a reference follows the one before it; escapes in shared strings are decoded.
    TEST_CHECK(hasText(table, 3, 1, "Bold text"));
    TEST_CHECK(hasText(table, 3, 2, "a\rb _x0041_ & < \xC3\xA9"));
    TEST_CHECK(hasText(table, 3, 4, ""));
    TEST_CHECK(hasNumber(table, 3, 5, SheetCellKind::Time, 0.5));
    TEST_CHECK(cellAt(table, 3, 3) == nullptr);
    TEST_CHECK(hasNumber(table, 3, 255, SheetCellKind::Boolean, 0.0));
    // IW4 is column 256, past the column limit.
    TEST_CHECK(cellAt(table, 3, 256) == nullptr);
    TEST_CHECK(table.rows[2].cells.size() == 5);
    TEST_CHECK(table.truncated);
    // A format id that only a differential format defines is a plain number; empty cells are left out.
    TEST_CHECK(hasNumber(table, 4, 0, SheetCellKind::Number, 3.0));
    TEST_CHECK(hasNumber(table, 4, 3, SheetCellKind::Date, 45000.25));
    TEST_CHECK(table.rows[3].cells.size() == 2);
    // A row without a reference follows the one before it, and a row of empty cells is left out.
    TEST_CHECK(hasNumber(table, 6, 0, SheetCellKind::Boolean, 0.0));
    TEST_CHECK((table.usedRange == SheetRange{.firstRow = 0, .firstColumn = 0, .lastRow = 6, .lastColumn = 255}));
  }

  void testSecondSheetWithPrefixedNames() {
    const auto book = readWith(workbook(), 1);
    TEST_CHECK(book.has_value());
    TEST_CHECK(book->sheetIndex == 1);
    TEST_CHECK((rowIndices(book->table) == std::vector<std::uint32_t>{1}));
    TEST_CHECK(hasNumber(book->table, 1, 1, SheetCellKind::Number, 1.0));
    TEST_CHECK(!book->table.truncated);
    TEST_CHECK((book->table.usedRange == SheetRange{.firstRow = 1, .firstColumn = 1, .lastRow = 1, .lastColumn = 1}));
  }

  void testDateSystem1904() {
    for (const std::string_view flag : {"1", "true"}) {
      const auto book = readWith(workbook(flag));
      TEST_CHECK(book.has_value());
      const SheetTable& table = book->table;
      TEST_CHECK(table.dateSystem == DateSystem::Date1904);
      TEST_CHECK(formatSheetDate(cellAt(table, 2, 2)->number, table.dateSystem) == "2027-03-16");
      TEST_CHECK(formatSheetDate(cellAt(table, 2, 3)->number, table.dateSystem) == "2027-03-16T12:00:00");
      // An ISO date cell gives the serial of the workbook's own system.
      TEST_CHECK(hasNumber(table, 4, 3, SheetCellKind::Date, 45000.25 - 1462.0));
      TEST_CHECK(formatSheetDate(cellAt(table, 4, 3)->number, table.dateSystem) == "2023-03-15T06:00:00");
    }
    TEST_CHECK(failsWith(readWith(workbook("yes")), SheetErrorKind::BadValue, "xl/workbook.xml"));
  }

  void testDateFormatting() {
    TEST_CHECK(formatSheetDate(1.0, DateSystem::Date1900) == "1900-01-01");
    TEST_CHECK(formatSheetDate(59.0, DateSystem::Date1900) == "1900-02-28");
    TEST_CHECK(formatSheetDate(60.0, DateSystem::Date1900) == "1900-02-29");
    TEST_CHECK(formatSheetDate(61.0, DateSystem::Date1900) == "1900-03-01");
    TEST_CHECK(formatSheetDate(60.25, DateSystem::Date1900) == "1900-02-29T06:00:00");
    TEST_CHECK(formatSheetDate(0.0, DateSystem::Date1900) == "1899-12-31");
    TEST_CHECK(formatSheetDate(-0.5, DateSystem::Date1900) == "1899-12-30T12:00:00");
    TEST_CHECK(formatSheetDate(2958465.0, DateSystem::Date1900) == "9999-12-31");
    TEST_CHECK(!formatSheetDate(2958466.0, DateSystem::Date1900).has_value());
    TEST_CHECK(formatSheetDate(0.0, DateSystem::Date1904) == "1904-01-01");
    TEST_CHECK(formatSheetDate(1.0, DateSystem::Date1904) == "1904-01-02");
    TEST_CHECK(formatSheetDate(59.0, DateSystem::Date1904) == "1904-02-29");
    // A time of day that rounds to midnight carries into the next day.
    TEST_CHECK(formatSheetDate(45000.9999999999, DateSystem::Date1900) == "2023-03-16");
    TEST_CHECK(formatSheetDate(45000.0 + (1.0 / 86400.0), DateSystem::Date1900) == "2023-03-15T00:00:01");
    TEST_CHECK(!formatSheetDate(NAN, DateSystem::Date1900).has_value());
    TEST_CHECK(!formatSheetDate(INFINITY, DateSystem::Date1904).has_value());
    TEST_CHECK(!formatSheetDate(1.0E12, DateSystem::Date1900).has_value());

    TEST_CHECK(formatSheetTime(0.75) == "18:00:00");
    TEST_CHECK(formatSheetTime(1.5) == "36:00:00");
    TEST_CHECK(formatSheetTime(0.0) == "00:00:00");
    TEST_CHECK(!formatSheetTime(-0.25).has_value());
    TEST_CHECK(!formatSheetTime(NAN).has_value());
  }

  void testIsoDatesAndNumbers() {
    TEST_CHECK(sheetSerialFromIso("2023-03-15", DateSystem::Date1900) == 45000.0);
    TEST_CHECK(sheetSerialFromIso("1900-02-28", DateSystem::Date1900) == 59.0);
    TEST_CHECK(sheetSerialFromIso("1900-03-01", DateSystem::Date1900) == 61.0);
    TEST_CHECK(sheetSerialFromIso("1900-01-01", DateSystem::Date1900) == 1.0);
    TEST_CHECK(sheetSerialFromIso("1904-01-01", DateSystem::Date1904) == 0.0);
    TEST_CHECK(sheetSerialFromIso("2023-03-15T18:00:00", DateSystem::Date1900) == 45000.75);
    TEST_NEAR(sheetSerialFromIso("2023-03-15T00:00:00.5", DateSystem::Date1900), 45000.0 + (0.5 / 86400.0), 1.0E-9);
    TEST_CHECK(sheetSerialFromIso("2024-02-29", DateSystem::Date1900).has_value());
    // A time-zone designator is passed over: serials carry no zone.
    TEST_CHECK(sheetSerialFromIso("2023-03-15T12:00:00Z", DateSystem::Date1900) == 45000.5);
    TEST_CHECK(sheetSerialFromIso("2023-03-15T12:00:00+02:00", DateSystem::Date1900) == 45000.5);
    TEST_NEAR(
        sheetSerialFromIso("2023-03-15T00:00:00.5-05:30", DateSystem::Date1900), 45000.0 + (0.5 / 86400.0), 1.0E-9
    );
    TEST_CHECK(sheetSerialFromIso("2023-03-15Z", DateSystem::Date1900) == 45000.0);
    TEST_CHECK(sheetSerialFromIso("2023-03-15-01:00", DateSystem::Date1900) == 45000.0);
    for (const std::string_view bad :
         {"", "2023-02-29", "2023-13-01", "2023-00-10", "2023-3-15", "2023/03/15", "2023-03-15T24:00:00",
          "2023-03-15T10:60:00", "2023-03-15T10:00", "2023-03-15T10:00:00.", "+023-03-15", "2023-03-15T10:00:00.5x",
          "2023-03-15ZZ", "2023-03-15T10:00:00+2", "2023-03-15T10:00:00+24:00", "2023-03-15T10:00:00+02:60",
          "2023-03-15T10:00:00 Z", "12:00:00"}) {
      TEST_CHECK(!sheetSerialFromIso(bad, DateSystem::Date1900).has_value());
    }
    TEST_CHECK(sheetTimeFromIso("12:00:00") == 0.5);
    TEST_CHECK(sheetTimeFromIso("18:00:00Z") == 0.75);
    TEST_NEAR(sheetTimeFromIso("00:00:00.5+01:00"), 0.5 / 86400.0, 1.0E-12);
    for (const std::string_view bad : {"", "12:00", "24:00:00", "12:00:00x", "2023-03-15", "2023-03-15T12:00:00"}) {
      TEST_CHECK(!sheetTimeFromIso(bad).has_value());
    }

    TEST_CHECK(parseSheetNumber(" 42.5\n") == 42.5);
    TEST_CHECK(parseSheetNumber("1E-3") == 0.001);
    TEST_CHECK(parseSheetNumber("-7") == -7.0);
    for (const std::string_view bad : {"", " ", "abc", "1x", "inf", "nan", "1e999", "+1"}) {
      TEST_CHECK(!parseSheetNumber(bad).has_value());
    }
  }

  void testMissingMembers() {
    for (const std::string_view member :
         {std::string_view("xl/workbook.xml"), std::string_view("xl/_rels/workbook.xml.rels"),
          std::string_view("xl/styles.xml"), kShared, kSheet1}) {
      Members members = workbook();
      members.erase(std::string(member));
      TEST_CHECK(failsWith(readWith(members), SheetErrorKind::MissingMember, member));
    }
    // Members the workbook does not name are optional, and a sheet that is not read may be missing.
    Members members = workbook();
    members["xl/_rels/workbook.xml.rels"] =
        "<Relationships>" + relationship("rId1", "worksheet", "worksheets/sheet1.xml") + "</Relationships>";
    members.erase("xl/styles.xml");
    members[std::string(kSheet1)] = worksheet(R"(<row r="1"><c r="A1" s="1"><v>2</v></c></row>)");
    const auto plain = readWith(members);
    TEST_CHECK(plain.has_value() && hasNumber(plain->table, 0, 0, SheetCellKind::Number, 2.0));
    TEST_CHECK(failsWith(readWith(members, 1), SheetErrorKind::MissingRelationship, "xl/_rels/workbook.xml.rels"));
  }

  void testSheetIndexAndRelationships() {
    TEST_CHECK(failsWith(readWith(workbook(), 2), SheetErrorKind::SheetIndex, "xl/workbook.xml"));
    // A target that starts with '/' is read from the archive root.
    Members members = workbook();
    members["xl/_rels/workbook.xml.rels"] =
        "<Relationships>" + relationship("rId1", "worksheet", "/sheets/one.xml") + "</Relationships>";
    members["sheets/one.xml"] = worksheet(R"(<row r="1"><c r="A1" t="inlineStr"><is><t>moved</t></is></c></row>)");
    const auto moved = readWith(members);
    TEST_CHECK(moved.has_value() && hasText(moved->table, 0, 0, "moved"));
  }

  void testSharedStringIndexThatDoesNotExist() {
    Members members = workbook();
    members[std::string(kSheet1)] = worksheet(R"(<row r="1"><c r="A1" t="s"><v>5</v></c></row>)");
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::SharedStringIndex, kSheet1));
    members.erase(std::string(kShared));
    members["xl/_rels/workbook.xml.rels"] =
        "<Relationships>" + relationship("rId1", "worksheet", "worksheets/sheet1.xml") + "</Relationships>";
    members[std::string(kSheet1)] = worksheet(R"(<row r="1"><c r="A1" t="s"><v>0</v></c></row>)");
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::SharedStringIndex, kSheet1));
  }

  void testBadValues() {
    for (const std::string_view rows :
         {R"(<row r="1"><c r="A1" t="s"><v>x</v></c></row>)", R"(<row r="1"><c r="A1" t="b"><v>2</v></c></row>)",
          R"(<row r="1"><c r="A1"><v>abc</v></c></row>)", R"(<row r="1"><c r="A1" t="d"><v>2023-02-30</v></c></row>)",
          R"(<row r="1"><c r="A1" t="q"><v>1</v></c></row>)", R"(<row r="1"><c r="A0"><v>1</v></c></row>)",
          R"(<row r="1"><c r="XFE1"><v>1</v></c></row>)", R"(<row r="1"><c r="a1"><v>1</v></c></row>)",
          R"(<row r="1"><c r="A2"><v>1</v></c></row>)", R"(<row r="0"/>)", R"(<row r="1048577"/>)", R"(<row r="x"/>)",
          R"(<row r="2"/><row r="1"/>)", R"(<row r="1"><c r="B1"><v>1</v></c><c r="A1"><v>1</v></c></row>)",
          R"(<row r="1"><c r="A1" s="z"><v>1</v></c></row>)"}) {
      Members members = workbook();
      members[std::string(kSheet1)] = worksheet(rows);
      TEST_CHECK(failsWith(readWith(members), SheetErrorKind::BadValue, kSheet1));
    }
    // A style index the workbook does not define reads as a plain number.
    Members members = workbook();
    members[std::string(kSheet1)] = worksheet(R"(<row r="1"><c r="A1" s="99"><v>1</v></c></row>)");
    const auto book = readWith(members);
    TEST_CHECK(book.has_value() && hasNumber(book->table, 0, 0, SheetCellKind::Number, 1.0));
  }

  void testBadWorkbookParts() {
    Members members = workbook();
    members["xl/_rels/workbook.xml.rels"] = R"(<Relationships><Relationship Id="rId1" Type="x/worksheet"/>)"
                                            "</Relationships>";
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::BadValue, "xl/_rels/workbook.xml.rels"));
    members = workbook();
    members["xl/workbook.xml"] = R"(<workbook><sheets><sheet sheetId="1" r:id="rId1"/></sheets></workbook>)";
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::BadValue, "xl/workbook.xml"));
    for (const std::string_view styles :
         {R"(<styleSheet><numFmts><numFmt numFmtId="x" formatCode="0"/></numFmts></styleSheet>)",
          R"(<styleSheet><cellXfs><xf numFmtId="-1"/></cellXfs></styleSheet>)"}) {
      members = workbook();
      members["xl/styles.xml"] = styles;
      TEST_CHECK(failsWith(readWith(members), SheetErrorKind::BadValue, "xl/styles.xml"));
    }
    members = workbook();
    members["xl/styles.xml"] = "<styleSheet>";
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::Xml, "xl/styles.xml", XmlError::Unterminated));
  }

  void testMalformedXml() {
    Members members = workbook();
    members[std::string(kSheet1)] = "<worksheet><sheetData><row r=\"1\">";
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::Xml, kSheet1, XmlError::Unterminated));
    members = workbook();
    members[std::string(kShared)] = "<sst><si><t>&bomb;</t></si></sst>";
    TEST_CHECK(failsWith(readWith(members), SheetErrorKind::Xml, kShared, XmlError::UnknownEntity));
  }

  void testRowAndCellLimits() {
    const auto rows = readWith(workbook(), 0, SheetLimits{.maxRows = 3});
    TEST_CHECK(rows.has_value());
    TEST_CHECK((rowIndices(rows->table) == std::vector<std::uint32_t>{0, 2}));
    TEST_CHECK(rows->table.truncated);

    const auto cells = readWith(workbook(), 0, SheetLimits{.maxCells = 4});
    TEST_CHECK(cells.has_value());
    TEST_CHECK((rowIndices(cells->table) == std::vector<std::uint32_t>{0, 2}));
    TEST_CHECK(cells->table.rows[1].cells.size() == 1);
    TEST_CHECK(cells->table.truncated);

    const auto text = readWith(workbook(), 0, SheetLimits{.maxTextBytes = 10});
    TEST_CHECK(text.has_value());
    // "Name" and "Inline" fit in ten bytes; "NameInline" does not.
    TEST_CHECK((rowIndices(text->table) == std::vector<std::uint32_t>{0}));
    TEST_CHECK(text->table.rows[0].cells.size() == 2);
    TEST_CHECK(text->table.truncated);

    const auto columns = readWith(workbook(), 1, SheetLimits{.maxColumns = 1});
    TEST_CHECK(columns.has_value() && columns->table.rows.empty() && columns->table.truncated);
    TEST_CHECK(!columns->table.usedRange.has_value());
  }

  void testCellTextLimits() {
    Members members = workbook();
    members[std::string(kSheet1)] =
        worksheet(R"(<row r="1"><c r="A1" t="s"><v>4</v></c><c r="B1" t="inlineStr"><is><t>abcdef</t></is></c></row>)");
    const auto clipped = readWith(members, 0, SheetLimits{.maxCellChars = 3});
    TEST_CHECK(clipped.has_value());
    TEST_CHECK(hasText(clipped->table, 0, 0, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
    TEST_CHECK(hasText(clipped->table, 0, 1, "abc"));
    TEST_CHECK(clipped->table.truncated);
    const auto whole = readWith(members, 0, SheetLimits{.maxCellChars = 6});
    TEST_CHECK(whole.has_value() && hasText(whole->table, 0, 1, "abcdef") && !whole->table.truncated);

    // A shared string past the kept ones leaves its cell out.
    const auto shared = readWith(workbook(), 0, SheetLimits{.maxSharedStrings = 1});
    TEST_CHECK(shared.has_value());
    TEST_CHECK(hasText(shared->table, 0, 0, "Name"));
    TEST_CHECK(cellAt(shared->table, 3, 1) == nullptr);
    TEST_CHECK(shared->table.truncated);
  }

  // Clock formats keep the time of day; elapsed formats ([h], [mm], [ss] or built-in 46) keep the duration.
  void testClockAndElapsedTimes() {
    Members members = workbook();
    members["xl/styles.xml"] =
        R"(<styleSheet><numFmts><numFmt numFmtId="164" formatCode="hh:mm:ss"/><numFmt numFmtId="165" formatCode="[h]:mm"/>)"
        R"(<numFmt numFmtId="166" formatCode="[mm]:ss"/><numFmt numFmtId="167" formatCode="[Red]h:mm"/></numFmts>)"
        R"(<cellXfs><xf numFmtId="0"/><xf numFmtId="20"/><xf numFmtId="46"/><xf numFmtId="164"/><xf numFmtId="165"/>)"
        R"(<xf numFmtId="166"/><xf numFmtId="167"/><xf numFmtId="18"/></cellXfs></styleSheet>)";
    members[std::string(kSheet1)] = worksheet(
        R"(<row r="1"><c s="1"><v>45000.5</v></c><c s="2"><v>1.5</v></c><c s="3"><v>45000.75</v></c>)"
        R"(<c s="4"><v>1.25</v></c><c s="5"><v>0.5</v></c><c s="6"><v>45000.25</v></c><c s="7"><v>2.5</v></c></row>)"
    );
    const auto book = readWith(members);
    TEST_CHECK(book.has_value());
    const SheetTable& table = book->table;
    const auto shown = [&table](std::uint32_t column) {
      const SheetCell* cell = cellAt(table, 0, column);
      return cell != nullptr && cell->kind == SheetCellKind::Time ? formatSheetTime(cell->number) : std::nullopt;
    };
    TEST_CHECK(shown(0) == "12:00:00");
    TEST_CHECK(shown(1) == "36:00:00");
    TEST_CHECK(shown(2) == "18:00:00");
    TEST_CHECK(shown(3) == "30:00:00");
    TEST_CHECK(shown(4) == "12:00:00");
    TEST_CHECK(shown(5) == "06:00:00");
    TEST_CHECK(shown(6) == "12:00:00");
  }

  // An ISO cell value may carry a time-zone designator, and a time-only value is a Time.
  void testIsoCellValues() {
    Members members = workbook();
    members[std::string(kSheet1)] = worksheet(
        R"(<row r="1"><c t="d"><v>2023-03-15T12:00:00Z</v></c><c t="d"><v>2023-03-15+02:00</v></c>)"
        R"(<c t="d"><v>18:00:00</v></c><c t="d"><v>06:00:00.5-05:00</v></c></row>)"
    );
    const auto book = readWith(members);
    TEST_CHECK(book.has_value());
    TEST_CHECK(hasNumber(book->table, 0, 0, SheetCellKind::Date, 45000.5));
    TEST_CHECK(hasNumber(book->table, 0, 1, SheetCellKind::Date, 45000.0));
    TEST_CHECK(hasNumber(book->table, 0, 2, SheetCellKind::Time, 0.75));
    TEST_CHECK(hasNumber(book->table, 0, 3, SheetCellKind::Time, 0.25 + (0.5 / 86400.0)));
  }

  // Cells past the column limit are dropped without copying their text, and a row at or past the row limit ends
  // a table that is already truncated, so the rest of the sheet is not read.
  void testDroppedCellsCostNothing() {
    Members members = workbook();
    members[std::string(kShared)] = std::format("<sst><si><t>{}</t></si></sst>", std::string(20'000, 'x'));
    std::string cells = R"(<c r="B1" t="s"><v>0</v></c>)";
    for (int k = 0; k < 2'000; ++k) {
      cells += R"(<c t="s"><v>0</v></c>)";
    }
    members[std::string(kSheet1)] = worksheet("<row r=\"1\">" + cells + "</row>");
    largeAllocations = 0;
    const auto wide = readWith(members, 0, SheetLimits{.maxColumns = 1});
    TEST_CHECK(largeAllocations < 100);
    TEST_CHECK(wide.has_value() && wide->table.rows.empty() && wide->table.truncated);

    members[std::string(kSheet1)] = worksheet(
        R"(<row r="1"><c r="A1"><v>1</v></c></row><row r="2"><c r="B2"><v>2</v></c></row>)"
        R"(<row r="3"><c r="B3"><v>3</v></c></row><row r="x"/>)"
    );
    const auto ended = readWith(members, 0, SheetLimits{.maxRows = 2, .maxColumns = 1});
    TEST_CHECK(ended.has_value() && ended->table.truncated);
    TEST_CHECK((rowIndices(ended->table) == std::vector<std::uint32_t>{0}));
    // An empty row past the limit of a table that lost nothing does not mark it truncated.
    members[std::string(kSheet1)] = worksheet(R"(<row r="1"><c r="A1"><v>1</v></c></row><row r="3"/>)");
    const auto whole = readWith(members, 0, SheetLimits{.maxRows = 2, .maxColumns = 1});
    TEST_CHECK(whole.has_value() && !whole->table.truncated);
  }
} // namespace

int main() {
  testWorkbookSheetsAndCells();
  testSparseCellsAndReferences();
  testSecondSheetWithPrefixedNames();
  testDateSystem1904();
  testDateFormatting();
  testIsoDatesAndNumbers();
  testMissingMembers();
  testSheetIndexAndRelationships();
  testSharedStringIndexThatDoesNotExist();
  testBadValues();
  testBadWorkbookParts();
  testMalformedXml();
  testRowAndCellLimits();
  testCellTextLimits();
  testClockAndElapsedTimes();
  testIsoCellValues();
  testDroppedCellsCostNothing();
  return 0;
}
