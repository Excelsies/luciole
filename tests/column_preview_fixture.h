#pragma once

#include "animation_files.h"
#include "table_fixture.h"
#include "temp_folder.h"
#include "test_check.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <glib.h>
#include <string>
#include <string_view>
#include <vector>

// One real file of each column preview kind in a private folder under the system's temporary directory, which goes
// with everything in it when the fixture goes. HOME and the XDG config, data and cache folders point inside it, so
// nothing under the user's home is read or written; the system's MIME database still types the files.
namespace luciole::test::preview {
  namespace fs = std::filesystem;
  using table::writeFile;

  inline std::string_view cssText() {
    return ":root {\n"
           "  --accent: #3584e4;\n"
           "  --warning: rgb(230, 97, 0);\n"
           "  --success: hsl(150 70% 40%);\n"
           "}\n\n"
           "/* buttons */\n"
           "button.primary {\n"
           "  color: #ffffff;\n"
           "  background: #9141ac;\n"
           "  border: 1px solid #e01b24;\n"
           "  padding: 4px 12px;\n"
           "}\n";
  }

  inline std::string_view markdownText() {
    return "# Release notes\n\n"
           "Columns ahead now preview **tables**, *models* and `archives`.\n\n"
           "## Changes\n\n"
           "- Faster folder measures\n"
           "- A new flow ribbon\n\n"
           "> Quoted words stay quiet.\n\n"
           "```sh\nmeson compile -C build\n```\n";
  }

  inline std::string csvText(std::size_t rows) {
    std::string text = "name,size,modified\n";
    for (std::size_t row = 0; row < rows; ++row) {
      text += std::format("file-{:03}.txt,{},2026-11-{:02}\n", row, (row + 1) * 1024, 1 + (row % 28));
    }
    return text;
  }

  // An ASCII STL of a tetrahedron, Z up.
  inline std::string_view stlText() {
    return "solid tetra\n"
           "facet normal 0 0 -1\n outer loop\n  vertex 0 0 0\n  vertex 1 0 0\n  vertex 0 1 0\n endloop\nendfacet\n"
           "facet normal 0 -1 0\n outer loop\n  vertex 0 0 0\n  vertex 0 0 1\n  vertex 1 0 0\n endloop\nendfacet\n"
           "facet normal -1 0 0\n outer loop\n  vertex 0 0 0\n  vertex 0 1 0\n  vertex 0 0 1\n endloop\nendfacet\n"
           "facet normal 1 1 1\n outer loop\n  vertex 1 0 0\n  vertex 0 0 1\n  vertex 0 1 0\n endloop\nendfacet\n"
           "endsolid tetra\n";
  }

  // Bytes that are neither text nor any known format.
  inline std::string binaryBytes() {
    std::string bytes;
    for (int i = 0; i < 300; ++i) {
      bytes.push_back(static_cast<char>((i * 37 + 11) % 256));
      bytes.push_back('\0');
    }
    return bytes;
  }

  // A mono 8 kHz PCM WAV of `samples` zero samples.
  inline std::string wavBytes(std::uint32_t samples) {
    const auto u32 = [](std::uint32_t value) {
      return std::string{
          static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
          static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)
      };
    };
    const auto u16 = [](std::uint16_t value) {
      return std::string{static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU)};
    };
    const std::uint32_t data = samples * 2;
    return "RIFF"
        + u32(36 + data)
        + "WAVEfmt "
        + u32(16)
        + u16(1)
        + u16(1)
        + u32(8000)
        + u32(16000)
        + u16(2)
        + u16(16)
        + "data"
        + u32(data)
        + std::string(data, '\0');
  }

  class PreviewFiles {
  public:
    PreviewFiles() {
      fs::create_directories(m_base / "home");
      for (const char* variable : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        g_setenv(variable, (m_base / "home").c_str(), TRUE);
      }
      m_folder = m_base / "files";
      fs::create_directories(m_folder / "folder");
      writeFile(m_folder / "style.css", cssText());
      writeFile(m_folder / "notes.md", markdownText());
      writeFile(m_folder / "data.csv", csvText(40));
      writeFile(m_folder / "data.tsv", "a\tb\n1\t2\n3\t4\n");
      writeFile(
          m_folder / "data.json",
          R"({"name": "luciole", "sizes": [1, 2, 3], "ok": true})"
          "\n"
      );
      writeFile(m_folder / "config.yaml", "name: luciole\nitems:\n  - one\n  - two\n");
      writeFile(m_folder / "setup.ini", "[main]\nkey = value\n");
      writeFile(m_folder / "feed.xml", "<?xml version=\"1.0\"?>\n<feed><title>News</title></feed>\n");
      writeFile(m_folder / "fix.diff", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n");
      writeFile(m_folder / "cube.stl", stlText());
      writeFile(m_folder / "blob.bin", binaryBytes());
      writeFile(
          m_folder / "id_test",
          "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXktdjEAAAAABG5vbmU=\n-----END OPENSSH PRIVATE KEY-----\n"
      );
      writeFile(m_folder / "paper.pdf", "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n1 0 obj\n<<>>\nendobj\ntrailer\n<<>>\n%%EOF\n");
      writeFile(m_folder / "tone.wav", wavBytes(1600));
      table::writeZip(
          m_folder / "book.xlsx",
          table::xlsxMembers(
              {{"Budget", sheetRows()}, {"Notes", std::format("<row r=\"1\">{}</row>", table::textCell("A1", "x"))}}
          )
      );
      table::writeZip(
          m_folder / "book.ods",
          table::odsMembers(
              {{"Sheet1",
                "<table:table-row><table:table-cell office:value-type=\"string\">"
                "<text:p>ods</text:p></table:table-cell></table:table-row>"}}
          )
      );
      table::writeZip(
          m_folder / "pack.zip",
          {{"docs/", ""},
           {"docs/readme.txt", "hello archive\n"},
           {"src/", ""},
           {"src/main.cpp", std::string(5000, 'c')},
           {"data.bin", std::string(70000, 'd')}}
      );
      const std::vector<::test::FlatFrame> frames{
          {.rgba = {224, 27, 36, 255}, .delayMs = 80},
          {.rgba = {53, 132, 228, 255}, .delayMs = 80},
          {.rgba = {46, 194, 126, 255}, .delayMs = 80},
      };
      TEST_CHECK(::test::writeBytes(m_folder / "spin.gif", ::test::animatedGif(96, 64, frames, 0)));
      TEST_CHECK(::test::writeBytes(m_folder / "still.png", ::test::stillPng(96, 64, {120, 80, 200, 255})));
    }
    PreviewFiles(const PreviewFiles&) = delete;
    PreviewFiles& operator=(const PreviewFiles&) = delete;
    PreviewFiles(PreviewFiles&&) = delete;
    PreviewFiles& operator=(PreviewFiles&&) = delete;

    const fs::path& folder() const noexcept { return m_folder; }

  private:
    static std::string sheetRows() {
      std::string rows = std::format(
          "<row r=\"1\">{}{}{}</row>", table::textCell("A1", "Item"), table::textCell("B1", "Cost"),
          table::textCell("C1", "Share")
      );
      constexpr std::array kItems{"Rent", "Food", "Travel", "Books", "Tools", "Music"};
      for (std::size_t i = 0; i < kItems.size(); ++i) {
        const auto row = i + 2;
        rows += std::format(
            "<row r=\"{}\">{}{}{}</row>", row, table::textCell(std::format("A{}", row), kItems[i]),
            table::numberCell(std::format("B{}", row), 120.5 * static_cast<double>(i + 1)),
            table::numberCell(std::format("C{}", row), 0.05 * static_cast<double>(i + 1))
        );
      }
      return rows;
    }

    TempFolder m_tempFolder{"luciole-column-preview"};
    fs::path m_base = m_tempFolder.path();
    fs::path m_folder;
  };
} // namespace luciole::test::preview
