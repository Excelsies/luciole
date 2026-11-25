#include "operations/archive_extra_formats.h"

#include "fs/archive_extra_types.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <charconv>
#include <string_view>

namespace {
  using namespace luciole;
  using namespace luciole::archive_work;
  // Members that index an ar archive: GNU's 64-bit symbol table and the BSD ones. libarchive consumes GNU's "/" and
  // "//" itself.
  constexpr std::array<std::string_view, 5> kIndexMembers{
      "/SYM64/", "__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"
  };
  bool isArchiveIndex(struct archive* reader, struct archive_entry* entry) {
    if ((archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_AR) {
      return false;
    }
    const char* name = archive_entry_pathname(entry);
    return name && std::ranges::find(kIndexMembers, std::string_view(name)) != kIndexMembers.end();
  }

  constexpr std::string_view kArMagic = "!<arch>\n";
  constexpr std::size_t kArHeaderBytes = 60;
  constexpr std::size_t kArSizeAt = 48, kArSizeWidth = 10;

  // Whether the file itself, before any decompression, starts as an ar archive.
  bool startsAsAr(Input& input) {
    std::array<char, kArMagic.size()> head{};
    const auto count = input.read(head);
    input.seek(0, SEEK_SET);
    return count == head.size() && std::string_view(head.data(), count) == kArMagic;
  }

  // A GNU "//" long-name table is the one member libarchive reads whole, with no limit of its own. A BSD "#1/N" name is
  // capped by libarchive itself at about 1 MiB, and a second table is refused by it.
  bool isNameTable(std::string_view header) {
    const auto name = header.substr(0, 16);
    return name.starts_with("//") && name.find_first_not_of(' ', 2) == std::string_view::npos;
  }

  // Walks the member headers of a plain ar file and refuses a name table that libarchive would hold beyond the
  // metadata limit. A header that does not parse ends the walk: libarchive reports it.
  void requireBoundedTables(Input& input, const Work& work) {
    const auto fileSize = static_cast<std::uint64_t>(input.seek(0, SEEK_END));
    std::uint64_t position = kArMagic.size();
    // Index members are skipped without counting as entries, so the walk allows as many of them again.
    const std::size_t bound = 2 * work.limits.maxEntries + 2;
    std::size_t seen = 0;
    for (; seen < bound && position + kArHeaderBytes <= fileSize; ++seen) {
      work.check();
      std::array<char, kArHeaderBytes> header{};
      input.seek(static_cast<std::int64_t>(position), SEEK_SET);
      if (input.read(header) != header.size()) {
        break;
      }
      const std::string_view view(header.data(), header.size());
      auto field = view.substr(kArSizeAt, kArSizeWidth);
      field = field.substr(0, field.find(' '));
      std::uint64_t size = 0;
      const auto parsed = std::from_chars(field.data(), field.data() + field.size(), size);
      if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()) {
        break;
      }
      if (isNameTable(view) && size > work.limits.maxMetadataBytes) {
        throwFileError(
            FileErrorKind::Unsupported, "The ar archive's long-name table is larger than the metadata limit."
        );
      }
      position += kArHeaderBytes + size + (size % 2);
    }
    input.seek(0, SEEK_SET);
    if (seen == bound) {
      throwFileError(FileErrorKind::Unsupported, "Archive has too many entries.");
    }
  }
} // namespace

void luciole::archive_work::enableExtraFilters(struct archive* reader, Work& work) {
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Lz4)) {
    requireArchive(archive_read_support_filter_lz4(reader), reader, work);
  }
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Compress)) {
    requireArchive(archive_read_support_filter_compress(reader), reader, work);
  }
}

void luciole::archive_work::enableExtraFormats(struct archive* reader, Input& input, Work& work) {
  // Only a file that is itself an ar archive: libarchive would also find one behind a decompression filter.
  if (startsAsAr(input)) {
    requireBoundedTables(input, work);
    requireArchive(archive_read_support_format_ar(reader), reader, work);
  }
  requireArchive(archive_read_support_format_cpio(reader), reader, work);
}

bool luciole::archive_work::extraFilterNeedsRawPass(int filterCode) {
  return filterCode == ARCHIVE_FILTER_LZ4 || filterCode == ARCHIVE_FILTER_COMPRESS;
}

int luciole::archive_work::nextHeader(struct archive* reader, struct archive_entry** entry, const Work& work) {
  for (std::size_t skipped = 0;; ++skipped) {
    const auto status = archive_read_next_header(reader, entry);
    if (status != ARCHIVE_OK || !isArchiveIndex(reader, *entry)) {
      return status;
    }
    work.check();
    if (skipped >= work.limits.maxEntries) {
      throwFileError(FileErrorKind::Unsupported, "Archive has too many entries.");
    }
  }
}

void luciole::archive_work::requireArchiveEnd(struct archive* reader, Input& input) {
  if ((archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_AR) {
    return;
  }
  if (archive_filter_bytes(reader, -1) != input.seek(0, SEEK_END)) {
    throwFileError(FileErrorKind::InvalidArgument, "The ar archive has a truncated trailing header.");
  }
}

void luciole::archive_work::requireHardlinkData(
    struct archive* reader, struct archive_entry* header, const std::string& target,
    const std::vector<ArchiveEntry>& entries, const std::map<std::string, std::size_t>& known
) {
  if ((archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_CPIO) {
    return;
  }
  const auto size = archive_entry_size_is_set(header) ? archive_entry_size(header) : 0;
  if (size <= 0) {
    return;
  }
  const auto found = known.find(target);
  const bool same = found != known.end()
      && entries[found->second].kind == ArchiveEntryKind::File
      && entries[found->second].size == static_cast<std::uint64_t>(size);
  if (!same) {
    throwFileError(
        FileErrorKind::Unsupported, "Archive hard link carries data that its target does not hold; not supported."
    );
  }
}
