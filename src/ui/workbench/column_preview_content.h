#pragma once

#include "formats/table_grid.h"
#include "fs/archive_container_types.h"
#include "fs/file_error.h"
#include "fs/location.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace luciole {
  struct ArchiveCatalog;

  // How a Workbench column previews a file.
  enum class ColumnPreviewKind : std::uint8_t {
    Still,     // the page's own preview over the slot: PDF pages, font sheets and everything else
    Picture,   // a still image, raster or vector, decoded by the preview decoder
    Text,      // text, code, JSON, YAML, TOML, INI, XML, diffs and Markdown, drawn as colored lines
    Table,     // CSV, TSV, XLSX and ODS
    Animation, // a GIF, PNG, APNG or WebP: played when its frames move, else shown as a Picture
    Media,     // video and audio, played, with the still as the poster
    Mesh,      // a 3D model to turn
    Archive,   // the archive's entries with their sizes
    Binary,    // a file of unknown type: its text when the bytes read as text, else the bytes in hex
  };

  // The kind for `file`, a regular file when `regular`, of content type `contentType` (empty when unknown).
  // `playback` says the build plays video and audio. First match: anything but a local regular file is Still; a
  // CSV, TSV, XLSX or ODS by type or name is Table; an STL, OBJ, PLY, glTF or GLB name is Mesh; an archive the
  // archive reader opens, or an archive container `openAsArchive` has, is Archive; GIF, PNG, APNG and WebP are
  // Animation; video and audio, by type or by name when the type is unknown, are Media with playback and Still without;
  // other images are Picture; text by type or by name is Text; an unknown or empty type is Binary; anything else is
  // Still. Where the type is not enough the loaders check the bytes: an image whose frames do not move, and private key
  // material, which is never shown.
  ColumnPreviewKind columnPreviewKind(
      const Location& file, std::string_view contentType, bool regular, bool playback,
      const ArchiveContainerSet& openAsArchive = {}
  );

  // What a column preview knows of its file; a fact is left out while unknown.
  struct ColumnPreviewFacts {
    std::optional<std::uint64_t> bytes = std::nullopt;
    std::optional<std::uint64_t> modified = std::nullopt;                    // seconds since the epoch
    std::optional<std::size_t> lines = std::nullopt;                         // of text
    std::optional<std::pair<std::size_t, std::size_t>> cells = std::nullopt; // rows and columns of a table
    std::optional<std::size_t> entries = std::nullopt;                       // of an archive
    std::optional<std::pair<int, int>> pixels = std::nullopt;                // width and height
    std::optional<double> seconds = std::nullopt;                            // playing time
    std::optional<std::size_t> triangles = std::nullopt;
    bool operator==(const ColumnPreviewFacts&) const = default;
  };

  // The facts in one line, in the order of the struct, joined by " · ": "3.0 KiB · 2026-11-18 09:30 · 120
  // lines". Sizes are written as the list writes them, the date in local time, counts with thousands separators
  // and times as m:ss or h:mm:ss. Values that cannot be shown (a negative or non-finite time, an empty picture, a
  // date past the calendar) are left out.
  std::string columnPreviewFactsText(const ColumnPreviewFacts& facts);

  // The entries the archive itself holds, leaving out the folders the reader adds for its members' paths. Such a
  // folder keeps ArchiveEntry's default mode, while a folder the archive stores carries its own.
  std::size_t archiveMemberCount(const ArchiveCatalog& catalog);

  // The archive's own entries in the catalog's order as a table with the columns Name and Size: the path inside the
  // archive, a folder's ending in '/', and the size where the archive states one. At most formats::kMaxGridRows
  // rows; more members mark the grid cut.
  formats::TableGrid archiveGrid(const ArchiveCatalog& catalog);

  // The sentence an archive preview shows when the reader refuses it: the reader's own words only where they name
  // what the archive needs (the 7-Zip command, or a multipart or encrypted RAR), "This archive is too large to
  // list." for a limit, and otherwise what went wrong by kind, in the words of the other previews.
  std::string archiveRefusal(const FileError& error);
} // namespace luciole
