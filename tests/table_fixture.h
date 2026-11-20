#pragma once

#include "temp_folder.h"
#include "test_check.h"

#include <archive.h>
#include <archive_entry.h>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace luciole::test::table {
  // A private folder under the system's temporary directory, removed with everything in it at the end.
  struct TemporaryFolder : TempFolder {
    TemporaryFolder() : TempFolder("luciole-table") {}
  };

  inline void writeFile(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(out.good());
  }

  // A member name and its bytes; a name ending in '/' is a folder.
  using ZipMember = std::pair<std::string, std::string>;

  // Writes a ZIP archive of deflated members in the given order. Without `sizes`, file members are streamed: their
  // sizes follow the data instead of preceding it.
  inline void writeZip(const std::filesystem::path& path, const std::vector<ZipMember>& members, bool sizes = true) {
    archive* out = archive_write_new();
    TEST_CHECK(out != nullptr);
    TEST_CHECK(archive_write_set_format_zip(out) == ARCHIVE_OK);
    TEST_CHECK(archive_write_zip_set_compression_deflate(out) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(out, path.c_str()) == ARCHIVE_OK);
    for (const auto& [name, bytes] : members) {
      archive_entry* entry = archive_entry_new();
      const bool folder = name.ends_with('/');
      archive_entry_set_pathname(entry, name.c_str());
      archive_entry_set_filetype(entry, folder ? AE_IFDIR : AE_IFREG);
      archive_entry_set_perm(entry, folder ? 0755 : 0644);
      if (folder || sizes) {
        archive_entry_set_size(entry, folder ? 0 : static_cast<la_int64_t>(bytes.size()));
      }
      TEST_CHECK(archive_write_header(out, entry) == ARCHIVE_OK);
      if (!folder && !bytes.empty()) {
        TEST_CHECK(archive_write_data(out, bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
      }
      archive_entry_free(entry);
    }
    TEST_CHECK(archive_write_close(out) == ARCHIVE_OK);
    archive_write_free(out);
  }

  inline std::string relationship(std::string_view id, std::string_view type, std::string_view target) {
    return std::format(
        R"(<Relationship Id="{}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/{}" )"
        R"(Target="{}"/>)",
        id, type, target
    );
  }

  inline std::string worksheet(std::string_view rows) {
    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
        R"(<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">)"
        R"(<sheetData>{}</sheetData></worksheet>)",
        rows
    );
  }

  // An XLSX package with one worksheet per entry of `sheets`: its name and its sheetData rows. Cells use inline
  // strings and numbers, so no shared strings or styles are needed.
  inline std::vector<ZipMember> xlsxMembers(const std::vector<std::pair<std::string, std::string>>& sheets) {
    std::string list;
    std::string relationships = "<Relationships>";
    std::vector<ZipMember> members;
    for (std::size_t i = 0; i < sheets.size(); ++i) {
      list += std::format(R"(<sheet name="{}" sheetId="{}" r:id="rId{}"/>)", sheets[i].first, i + 1, i + 1);
      relationships +=
          relationship(std::format("rId{}", i + 1), "worksheet", std::format("worksheets/sheet{}.xml", i + 1));
      members.emplace_back(std::format("xl/worksheets/sheet{}.xml", i + 1), worksheet(sheets[i].second));
    }
    members.emplace_back("[Content_Types].xml", "<Types/>");
    members.emplace_back("xl/media/", "");
    members.emplace_back("xl/media/image1.png", std::string(4096, '\x89'));
    members.emplace_back(
        "xl/workbook.xml",
        R"(<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" )"
        R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>)"
            + list
            + "</sheets></workbook>"
    );
    members.emplace_back("xl/_rels/workbook.xml.rels", relationships + "</Relationships>");
    return members;
  }

  // An inline-string cell and a number cell, as sheetData cell markup.
  inline std::string textCell(std::string_view reference, std::string_view text) {
    return std::format(R"(<c r="{}" t="inlineStr"><is><t>{}</t></is></c>)", reference, text);
  }
  inline std::string numberCell(std::string_view reference, double value) {
    return std::format(R"(<c r="{}"><v>{}</v></c>)", reference, value);
  }

  // An ODS package whose content holds one table per entry of `tables`: its name and its table rows markup.
  inline std::vector<ZipMember> odsMembers(const std::vector<std::pair<std::string, std::string>>& tables) {
    std::string body;
    for (const auto& [name, rows] : tables) {
      body += std::format(R"(<table:table table:name="{}">{}</table:table>)", name, rows);
    }
    return {
        {"mimetype", "application/vnd.oasis.opendocument.spreadsheet"},
        {"content.xml",
         R"(<office:document-content xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" )"
         R"(xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" )"
         R"(xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"><office:body><office:spreadsheet>)"
             + body
             + "</office:spreadsheet></office:body></office:document-content>"},
    };
  }
} // namespace luciole::test::table
