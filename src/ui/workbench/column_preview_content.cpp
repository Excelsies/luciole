#include "ui/workbench/column_preview_content.h"

#include "formats/file_kind.h"
#include "fs/archive_type.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "thumbnails/mesh_preview.h"
#include "thumbnails/table_loader.h"
#include "workbench/format.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <glib.h>
#include <memory>
#include <vector>

namespace luciole {
  namespace {
    using namespace std::string_view_literals;

    constexpr auto kAnimatedTypes = std::array{"image/gif"sv, "image/apng"sv, "image/png"sv, "image/webp"sv};

    // Text types the "text/" prefix and the name do not cover. Key files are text so their loader refuses them.
    constexpr auto kTextTypes = std::array{
        "application/json"sv,  "application/xml"sv,           "application/yaml"sv,      "application/x-yaml"sv,
        "application/toml"sv,  "application/x-shellscript"sv, "application/x-desktop"sv, "application/sql"sv,
        "application/x-nix"sv, "application/x-pem-file"sv,    "application/x-pem-key"sv, "application/pgp-keys"sv,
    };

    // What GIO reports when nothing names the type.
    constexpr auto kUnknownTypes = std::array{""sv, "application/octet-stream"sv, "application/x-zerosize"sv};

    constexpr auto kSeparator = " · "sv;

    constexpr auto kArchiveUnreadable = "This archive could not be read."sv;
    // How the reader's sentences that name what an archive needs begin.
    constexpr auto kArchiveNeeds = std::array{"Encrypted 7z archives need "sv, "Multipart RAR "sv, "Encrypted RAR "sv};

    bool listed(std::span<const std::string_view> types, std::string_view type) {
      return std::ranges::find(types, type) != types.end();
    }

    bool media(std::string_view type, const formats::FileKind& named) {
      if (type.starts_with("video/") || type.starts_with("audio/")) {
        return true;
      }
      return listed(kUnknownTypes, type)
          && (named.content == formats::ContentKind::Video || named.content == formats::ContentKind::Audio);
    }

    bool text(std::string_view type, const formats::FileKind& named) {
      return named.content == formats::ContentKind::Text
          || type.starts_with("text/")
          || listed(kTextTypes, type)
          || type.ends_with("+json")
          || type.ends_with("+xml");
    }

    std::string counted(std::size_t count, std::string_view one, std::string_view many) {
      return std::format("{} {}", workbench::countText(static_cast<double>(count)), count == 1 ? one : many);
    }

    // Local time as the side preview writes it; empty past what GLib's calendar holds.
    std::string dateText(std::uint64_t seconds) {
      if (seconds > static_cast<std::uint64_t>(G_MAXINT64)) {
        return {};
      }
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_from_unix_local(static_cast<gint64>(seconds)), &g_date_time_unref
      );
      if (!date) {
        return {};
      }
      const GString formatted(g_date_time_format(date.get(), "%Y-%m-%d %H:%M"));
      return formatted ? std::string(formatted.get()) : std::string{};
    }

    // m:ss below an hour, else h:mm:ss; empty for a negative, non-finite or absurd time.
    std::string durationText(double seconds) {
      constexpr double kLongest = 1e9;
      if (!std::isfinite(seconds) || seconds < 0 || seconds > kLongest) {
        return {};
      }
      const auto whole = static_cast<std::uint64_t>(seconds);
      const auto hours = whole / 3600;
      const auto minutes = (whole / 60) % 60;
      const auto rest = whole % 60;
      return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, rest) : std::format("{}:{:02}", minutes, rest);
    }

    // Not a folder the reader added for a member's path: those keep the default mode.
    bool member(const ArchiveEntry& entry) {
      return entry.kind != ArchiveEntryKind::Directory || entry.mode != ArchiveEntry{}.mode;
    }
  } // namespace

  ColumnPreviewKind columnPreviewKind(const Location& file, std::string_view contentType, bool regular, bool playback) {
    const auto name = file.basename();
    if (!regular || !name || !file.nativePath()) {
      return ColumnPreviewKind::Still;
    }
    if (tableSource(*name, contentType)) {
      return ColumnPreviewKind::Table;
    }
    if (meshFormat(*name)) {
      return ColumnPreviewKind::Mesh;
    }
    if (isSupportedArchive(file, contentType)) {
      return ColumnPreviewKind::Archive;
    }
    if (listed(kAnimatedTypes, contentType)) {
      return ColumnPreviewKind::Animation;
    }
    const auto named = formats::fileKind(*name);
    if (media(contentType, named)) {
      return playback ? ColumnPreviewKind::Media : ColumnPreviewKind::Still;
    }
    if (contentType.starts_with("image/")) {
      return ColumnPreviewKind::Picture;
    }
    if (text(contentType, named)) {
      return ColumnPreviewKind::Text;
    }
    return listed(kUnknownTypes, contentType) ? ColumnPreviewKind::Binary : ColumnPreviewKind::Still;
  }

  std::string columnPreviewFactsText(const ColumnPreviewFacts& facts) {
    std::vector<std::string> parts;
    if (facts.bytes) {
      parts.push_back(workbench::sizeText(static_cast<double>(*facts.bytes)));
    }
    if (facts.modified) {
      parts.push_back(dateText(*facts.modified));
    }
    if (facts.lines) {
      parts.push_back(counted(*facts.lines, "line", "lines"));
    }
    if (facts.cells) {
      parts.push_back(
          counted(facts.cells->first, "row", "rows") + ", " + counted(facts.cells->second, "column", "columns")
      );
    }
    if (facts.entries) {
      parts.push_back(counted(*facts.entries, "entry", "entries"));
    }
    if (facts.pixels && facts.pixels->first > 0 && facts.pixels->second > 0) {
      parts.push_back(std::format("{} × {}", facts.pixels->first, facts.pixels->second));
    }
    if (facts.seconds) {
      parts.push_back(durationText(*facts.seconds));
    }
    if (facts.triangles) {
      parts.push_back(counted(*facts.triangles, "triangle", "triangles"));
    }
    std::string line;
    for (const auto& part : parts) {
      if (part.empty()) {
        continue;
      }
      if (!line.empty()) {
        line += kSeparator;
      }
      line += part;
    }
    return line;
  }

  std::size_t archiveMemberCount(const ArchiveCatalog& catalog) {
    return static_cast<std::size_t>(std::ranges::count_if(catalog.entries, member));
  }

  formats::TableGrid archiveGrid(const ArchiveCatalog& catalog) {
    formats::TableGridBuilder builder(archiveMemberCount(catalog), 2);
    std::size_t row = 0;
    // Bounded by the catalog's entries.
    for (const auto& entry : catalog.entries) {
      if (row == builder.rowCount()) {
        break;
      }
      if (!member(entry)) {
        continue;
      }
      const bool folder = entry.kind == ArchiveEntryKind::Directory;
      const auto name = folder && !entry.path.ends_with('/') ? entry.path + '/' : entry.path;
      (void)builder.set(row, 0, formats::GridCellKind::Text, formats::gridText(name));
      if (entry.size && !folder) {
        (void)builder.set(row, 1, formats::GridCellKind::Number, workbench::sizeText(static_cast<double>(*entry.size)));
      }
      ++row;
    }
    auto grid = builder.finish();
    grid.columnNames = {"Name", "Size"};
    return grid;
  }

  std::string archiveRefusal(const FileError& error) {
    switch (error.kind) {
    case FileErrorKind::Unsupported:
    case FileErrorKind::Unavailable:
      if (std::ranges::any_of(kArchiveNeeds, [&](std::string_view head) { return error.message.starts_with(head); })) {
        return error.message;
      }
      if (error.message.contains("limit") || error.message.contains("too many")) {
        return "This archive is too large to list.";
      }
      return std::string(kArchiveUnreadable);
    case FileErrorKind::Missing:
      return "This archive, or a part of it, is missing.";
    case FileErrorKind::Denied:
      return "This archive cannot be opened.";
    case FileErrorKind::Changed:
      return "This archive changed while it was read.";
    case FileErrorKind::InvalidArgument:
    case FileErrorKind::NameTooLong:
      return "This archive is damaged, so its entries are not listed.";
    default:
      return std::string(kArchiveUnreadable);
    }
  }
} // namespace luciole
