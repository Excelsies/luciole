#include "fs/archive_type.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "operations/archive_service.h"
#include "test_check.h"
#include "ui/workbench/column_preview_content.h"

#include <array>
#include <cstdint>
#include <glib.h>
#include <limits>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using luciole::ColumnPreviewFacts;
  using luciole::ColumnPreviewKind;
  using luciole::Location;
  using Kind = ColumnPreviewKind;

  struct Row {
    std::string_view name;
    std::string_view type;
    Kind withPlayback;
    Kind withoutPlayback;
  };

  // One or more rows per family of the preview table in the Workbench design.
  constexpr std::array kRows{
      // Raster and vector images: the picture, drawn in the slot.
      Row{"photo.jpg", "image/jpeg", Kind::Picture, Kind::Picture},
      Row{"logo.svg", "image/svg+xml", Kind::Picture, Kind::Picture},
      Row{"scan.tiff", "image/tiff", Kind::Picture, Kind::Picture},
      Row{"raw.heic", "image/heif", Kind::Picture, Kind::Picture},
      // Animated images: every type that can move is played, and turns out still by its frames.
      Row{"spin.gif", "image/gif", Kind::Animation, Kind::Animation},
      Row{"spin.png", "image/png", Kind::Animation, Kind::Animation},
      Row{"spin.apng", "image/apng", Kind::Animation, Kind::Animation},
      Row{"spin.webp", "image/webp", Kind::Animation, Kind::Animation},
      // Video and audio: playback when the build has it, else the still.
      Row{"clip.mp4", "video/mp4", Kind::Media, Kind::Still},
      Row{"clip.webm", "video/webm", Kind::Media, Kind::Still},
      Row{"tone.ogg", "audio/ogg", Kind::Media, Kind::Still},
      Row{"song.flac", "audio/flac", Kind::Media, Kind::Still},
      Row{"untyped.mkv", "", Kind::Media, Kind::Still},
      // Text, code, JSON, YAML, TOML, INI, XML and diffs.
      Row{"notes.txt", "text/plain", Kind::Text, Kind::Text},
      Row{"main.cpp", "text/x-c++src", Kind::Text, Kind::Text},
      Row{"style.css", "text/css", Kind::Text, Kind::Text},
      Row{"page.html", "text/html", Kind::Text, Kind::Text},
      Row{"data.json", "application/json", Kind::Text, Kind::Text},
      Row{"site.webmanifest", "application/manifest+json", Kind::Text, Kind::Text},
      Row{"config.yaml", "application/yaml", Kind::Text, Kind::Text},
      Row{"compose.yml", "application/x-yaml", Kind::Text, Kind::Text},
      Row{"Cargo.toml", "application/toml", Kind::Text, Kind::Text},
      Row{"setup.ini", "text/plain", Kind::Text, Kind::Text},
      Row{"feed.xml", "application/xml", Kind::Text, Kind::Text},
      Row{"news.rss", "application/rss+xml", Kind::Text, Kind::Text},
      Row{"fix.diff", "text/x-patch", Kind::Text, Kind::Text},
      Row{"run.sh", "application/x-shellscript", Kind::Text, Kind::Text},
      Row{"app.desktop", "application/x-desktop", Kind::Text, Kind::Text},
      Row{"query.sql", "application/sql", Kind::Text, Kind::Text},
      Row{"Makefile", "text/x-makefile", Kind::Text, Kind::Text},
      Row{"flake.nix", "application/octet-stream", Kind::Text, Kind::Text}, // the name knows it
      // Markdown: rendered blocks in the same view.
      Row{"README.md", "text/markdown", Kind::Text, Kind::Text},
      Row{"guide.markdown", "", Kind::Text, Kind::Text},
      // CSV and TSV, by type or by name.
      Row{"data.csv", "text/csv", Kind::Table, Kind::Table},
      Row{"export.csv", "text/plain", Kind::Table, Kind::Table},
      Row{"data.tsv", "text/tab-separated-values", Kind::Table, Kind::Table},
      // XLSX and ODS.
      Row{"book.xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", Kind::Table, Kind::Table},
      Row{"book.ods", "application/vnd.oasis.opendocument.spreadsheet", Kind::Table, Kind::Table},
      // Documents and presentations: the first page, as the page's own preview draws it.
      Row{"paper.pdf", "application/pdf", Kind::Still, Kind::Still},
      Row{"letter.docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document", Kind::Still,
          Kind::Still},
      Row{"slides.odp", "application/vnd.oasis.opendocument.presentation", Kind::Still, Kind::Still},
      // Fonts: the font sheet.
      Row{"Inter.ttf", "font/ttf", Kind::Still, Kind::Still},
      Row{"Inter.woff2", "font/woff2", Kind::Still, Kind::Still},
      // Archives: a listing of the entries.
      Row{"pack.zip", "application/zip", Kind::Archive, Kind::Archive},
      Row{"pack.tar", "application/x-tar", Kind::Archive, Kind::Archive},
      Row{"pack.7z", "application/x-7z-compressed", Kind::Archive, Kind::Archive},
      Row{"untyped.zip", "", Kind::Archive, Kind::Archive},
      // 3D models: the orbit view, by name.
      Row{"cube.stl", "model/stl", Kind::Mesh, Kind::Mesh},
      Row{"cube.obj", "model/obj", Kind::Mesh, Kind::Mesh},
      Row{"cube.ply", "application/octet-stream", Kind::Mesh, Kind::Mesh},
      Row{"scene.gltf", "model/gltf+json", Kind::Mesh, Kind::Mesh},
      Row{"scene.glb", "model/gltf-binary", Kind::Mesh, Kind::Mesh},
      // Everything else: facts and hex for a file of unknown type, the page's own preview for the rest.
      Row{"blob.bin", "application/octet-stream", Kind::Binary, Kind::Binary},
      Row{"noname", "", Kind::Binary, Kind::Binary},
      Row{"empty", "application/x-zerosize", Kind::Binary, Kind::Binary},
      Row{"a.out", "application/x-executable", Kind::Still, Kind::Still},
      Row{"part.3mf", "model/3mf", Kind::Still, Kind::Still},
      Row{"disk.iso", "application/vnd.efi.iso", Kind::Still, Kind::Still},
      // Key files are read as text so their loader can refuse them; the bytes decide, never the name.
      Row{"server.pem", "application/x-pem-file", Kind::Text, Kind::Text},
      Row{"signing.asc", "application/pgp-keys", Kind::Text, Kind::Text},
      // Certificate and key containers a MIME database may type in many ways: all are read, and the bytes decide.
      Row{"bundle.pem", "application/pkcs7-mime", Kind::Text, Kind::Text},
      Row{"chain.crt", "application/pkix-cert", Kind::Text, Kind::Text},
      Row{"store.p12", "application/x-pkcs12", Kind::Text, Kind::Text},
      Row{"id_ed25519", "application/octet-stream", Kind::Binary, Kind::Binary},
  };

  Location file(std::string_view name) { return Location::fromPath("/srv/files/" + std::string(name)); }

  void everyRow() {
    for (const auto& row : kRows) {
      const auto with = luciole::columnPreviewKind(file(row.name), row.type, true, true, {});
      const auto without = luciole::columnPreviewKind(file(row.name), row.type, true, false, {});
      if (with != row.withPlayback || without != row.withoutPlayback) {
        std::println("{} ({}): {} and {}", row.name, row.type, static_cast<int>(with), static_cast<int>(without));
      }
      TEST_CHECK(with == row.withPlayback);
      TEST_CHECK(without == row.withoutPlayback);
    }
  }

  // Compressed tarballs are listed only where the reader can unpack their filter.
  void compressedArchivesFollowTheReader() {
    const auto kind = luciole::columnPreviewKind(file("pack.tar.gz"), "application/x-compressed-tar", true, true, {});
    const bool gzip = luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Gzip);
    TEST_CHECK(kind == (gzip ? Kind::Archive : Kind::Still));
  }

  // Folders, devices and files that are not local keep the page's own preview.
  void onlyLocalRegularFiles() {
    TEST_CHECK(luciole::columnPreviewKind(file("notes.txt"), "text/plain", false, true, {}) == Kind::Still);
    TEST_CHECK(luciole::columnPreviewKind(file("pack.zip"), "inode/directory", false, true, {}) == Kind::Still);
    const auto remote = Location::fromUri("trash:///notes.txt");
    TEST_CHECK(!remote.nativePath());
    TEST_CHECK(luciole::columnPreviewKind(remote, "text/plain", true, true, {}) == Kind::Still);
    TEST_CHECK(luciole::columnPreviewKind(Location::fromPath("/"), "inode/directory", true, true, {}) == Kind::Still);
  }

  void factsLine() {
    TEST_CHECK(luciole::columnPreviewFactsText({}).empty());
    TEST_CHECK(luciole::columnPreviewFactsText({.bytes = 0}) == "0 bytes");
    TEST_CHECK(luciole::columnPreviewFactsText({.bytes = 3072}) == "3.0\u00A0KiB");
    // 2026-11-18 09:30:00 UTC; the test runs in UTC.
    TEST_CHECK(luciole::columnPreviewFactsText({.bytes = 1, .modified = 1'794'994'200}) == "1 byte · 2026-11-18 09:30");
    TEST_CHECK(luciole::columnPreviewFactsText({.lines = 1}) == "1 line");
    TEST_CHECK(luciole::columnPreviewFactsText({.lines = 12'000}) == "12,000 lines");
    TEST_CHECK(luciole::columnPreviewFactsText({.cells = {{1, 1}}}) == "1 row, 1 column");
    TEST_CHECK(luciole::columnPreviewFactsText({.cells = {{2500, 4}}}) == "2,500 rows, 4 columns");
    TEST_CHECK(luciole::columnPreviewFactsText({.entries = 1}) == "1 entry");
    TEST_CHECK(luciole::columnPreviewFactsText({.entries = 30}) == "30 entries");
    TEST_CHECK(luciole::columnPreviewFactsText({.pixels = {{640, 480}}}) == "640 × 480");
    TEST_CHECK(luciole::columnPreviewFactsText({.seconds = 65.4}) == "1:05");
    TEST_CHECK(luciole::columnPreviewFactsText({.seconds = 3723}) == "1:02:03");
    TEST_CHECK(luciole::columnPreviewFactsText({.triangles = 1}) == "1 triangle");
    TEST_CHECK(luciole::columnPreviewFactsText({.triangles = 1'000'000}) == "1,000,000 triangles");
    TEST_CHECK(
        luciole::columnPreviewFactsText({.bytes = 2048, .lines = 3, .triangles = 12})
        == "2.0\u00A0KiB · 3 lines · 12 triangles"
    );
    // Values that cannot be shown are left out.
    for (const double bad : {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      TEST_CHECK(luciole::columnPreviewFactsText({.seconds = bad}).empty());
    }
    TEST_CHECK(luciole::columnPreviewFactsText({.pixels = {{0, 480}}}).empty());
    TEST_CHECK(luciole::columnPreviewFactsText({.modified = std::numeric_limits<std::uint64_t>::max()}).empty());
  }

  void archiveListing() {
    // A folder the archive stores carries its own mode; one the reader adds for a member's path keeps the default.
    const auto entry = [](std::string path, std::optional<std::uint64_t> size, bool folder, bool stored = true) {
      luciole::ArchiveEntry made;
      made.path = std::move(path);
      made.size = size;
      made.kind = folder ? luciole::ArchiveEntryKind::Directory : luciole::ArchiveEntryKind::File;
      if (folder && stored) {
        made.mode = 0755;
      }
      return made;
    };
    const auto empty = [] {
      return luciole::ArchiveCatalog{
          .source = file("pack.zip"),
          .stamp = {},
          .format = {},
          .entries = {},
          .expandedBytes = 0,
          .encrypted = false,
          .compression = {},
          .archiveBytes = 0,
          .single = false
      };
    };
    auto catalog = empty();
    catalog.entries.push_back(entry("docs/", std::nullopt, true));
    catalog.entries.push_back(entry("docs/a.txt", 3072, false));
    catalog.entries.push_back(entry("unknown.bin", std::nullopt, false));
    catalog.entries.push_back(entry("line\nbreak", 1, false));
    catalog.entries.push_back(entry("lib", 0, true)); // catalogs name folders without the slash
    const auto grid = luciole::archiveGrid(catalog);
    TEST_CHECK(grid.rowCount == 5 && grid.columnCount == 2);
    TEST_CHECK((grid.columnNames == std::vector<std::string>{"Name", "Size"}));
    TEST_CHECK(luciole::formats::gridCellText(grid, 0, 0) == "docs/");
    TEST_CHECK(luciole::formats::gridCellText(grid, 0, 1).empty());
    TEST_CHECK(luciole::formats::gridCellText(grid, 1, 0) == "docs/a.txt");
    TEST_CHECK(luciole::formats::gridCellText(grid, 1, 1) == "3.0\u00A0KiB");
    TEST_CHECK(luciole::formats::gridCellKind(grid, 1, 1) == luciole::formats::GridCellKind::Number);
    TEST_CHECK(luciole::formats::gridCellText(grid, 2, 1).empty());
    TEST_CHECK(luciole::formats::gridCellText(grid, 3, 0) == "line break"); // one line per cell
    TEST_CHECK(luciole::formats::gridCellText(grid, 4, 0) == "lib/");
    TEST_CHECK(luciole::formats::gridCellText(grid, 4, 1).empty());
    TEST_CHECK(!grid.truncated);
    TEST_CHECK(luciole::archiveMemberCount(catalog) == 5);

    // Only the archive's own members are listed and counted, not the folders the reader adds for their paths.
    auto loose = empty();
    loose.entries.push_back(entry("docs", std::nullopt, true));
    loose.entries.push_back(entry("docs/readme.txt", 14, false));
    loose.entries.push_back(entry("src", std::nullopt, true, false));
    loose.entries.push_back(entry("src/main.cpp", 5000, false));
    loose.entries.push_back(entry("data.bin", 70000, false));
    const auto members = luciole::archiveGrid(loose);
    TEST_CHECK(members.rowCount == 4 && luciole::archiveMemberCount(loose) == 4);
    TEST_CHECK(luciole::formats::gridCellText(members, 1, 0) == "docs/readme.txt");
    TEST_CHECK(luciole::formats::gridCellText(members, 2, 0) == "src/main.cpp");
    TEST_CHECK(luciole::formats::gridCellText(members, 3, 0) == "data.bin");

    auto large = empty();
    large.entries.resize(luciole::formats::kMaxGridRows + 5, entry("f", 1, false));
    const auto cut = luciole::archiveGrid(large);
    TEST_CHECK(cut.rowCount == luciole::formats::kMaxGridRows);
    TEST_CHECK(cut.truncated && cut.rowsCut);
    TEST_CHECK(luciole::archiveMemberCount(large) == luciole::formats::kMaxGridRows + 5);
    // Added folders ahead of the members do not take rows from them.
    auto deep = empty();
    deep.entries.resize(luciole::formats::kMaxGridRows, entry("d", std::nullopt, true, false));
    deep.entries.push_back(entry("d/f", 1, false));
    const auto deepGrid = luciole::archiveGrid(deep);
    TEST_CHECK(deepGrid.rowCount == 1 && !deepGrid.truncated);
    TEST_CHECK(luciole::formats::gridCellText(deepGrid, 0, 0) == "d/f");

    const auto none = empty();
    TEST_CHECK(luciole::archiveGrid(none).rowCount == 0 && luciole::archiveMemberCount(none) == 0);
  }

  void archiveRefusals() {
    using luciole::FileErrorKind;
    const auto said = [](FileErrorKind kind, std::string message) {
      return luciole::archiveRefusal({.kind = kind, .message = std::move(message)});
    };
    // The reader's own sentence where it names what the archive needs.
    constexpr std::string_view kSevenZip = "Encrypted 7z archives need the 7-Zip command (7zz or 7z) installed.";
    TEST_CHECK(said(FileErrorKind::Unavailable, std::string(kSevenZip)) == kSevenZip);
    TEST_CHECK(
        said(FileErrorKind::Unsupported, "Multipart RAR archives are not supported yet.")
        == "Multipart RAR archives are not supported yet."
    );
    TEST_CHECK(
        said(FileErrorKind::Unsupported, "Encrypted RAR archives are not supported by the archive provider.")
        == "Encrypted RAR archives are not supported by the archive provider."
    );
    // A limit says the archive is too large.
    constexpr std::string_view kTooLarge = "This archive is too large to list.";
    TEST_CHECK(said(FileErrorKind::Unsupported, "Archive has too many entries.") == kTooLarge);
    TEST_CHECK(said(FileErrorKind::Unsupported, "The archive provider output exceeds its limit.") == kTooLarge);
    TEST_CHECK(said(FileErrorKind::Unsupported, "RAR trailing headers exceed the metadata limit.") == kTooLarge);
    TEST_CHECK(said(FileErrorKind::Unsupported, "Archive exceeds the expanded-size limit.") == kTooLarge);
    // Any other reason is the reader's own business.
    constexpr std::string_view kUnreadable = "This archive could not be read.";
    TEST_CHECK(said(FileErrorKind::Unavailable, "Could not start the archive provider.") == kUnreadable);
    TEST_CHECK(said(FileErrorKind::Unavailable, "The 7-Zip command path must be absolute.") == kUnreadable);
    TEST_CHECK(said(FileErrorKind::Unsupported, "Archive data exceeds its expected size.") == kUnreadable);
    TEST_CHECK(said(FileErrorKind::Unsupported, "Archive hard-link chain is too deep.") == kUnreadable);
    TEST_CHECK(said(FileErrorKind::Unsupported, "") == kUnreadable);
    TEST_CHECK(said(FileErrorKind::Other, "Input/output error") == kUnreadable);
    // Other kinds say what happened in the same voice.
    TEST_CHECK(said(FileErrorKind::Missing, "No such file") == "This archive, or a part of it, is missing.");
    TEST_CHECK(said(FileErrorKind::Denied, "Permission denied") == "This archive cannot be opened.");
    TEST_CHECK(said(FileErrorKind::Changed, {}) == "This archive changed while it was read.");
    TEST_CHECK(
        said(FileErrorKind::InvalidArgument, "Archive has an invalid hard-link target.")
        == "This archive is damaged, so its entries are not listed."
    );
  }
} // namespace

int main() {
  g_setenv("TZ", "UTC", TRUE);
  everyRow();
  compressedArchivesFollowTheReader();
  onlyLocalRegularFiles();
  factsLine();
  archiveListing();
  archiveRefusals();
  std::println("column-preview-kind: {} rows checked", kRows.size());
  return 0;
}
