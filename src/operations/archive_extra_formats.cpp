#include "operations/archive_extra_formats.h"

#include "fs/archive_extra_types.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
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
} // namespace

void luciole::archive_work::enableExtraFilters(struct archive* reader, Work& work) {
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Lz4)) {
    requireArchive(archive_read_support_filter_lz4(reader), reader, work);
  }
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Compress)) {
    requireArchive(archive_read_support_filter_compress(reader), reader, work);
  }
}

void luciole::archive_work::enableExtraFormats(struct archive* reader, Work& work) {
  requireArchive(archive_read_support_format_ar(reader), reader, work);
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
  if ((archive_format(reader) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_AR || archive_filter_count(reader) != 1) {
    return;
  }
  if (archive_filter_bytes(reader, -1) != input.seek(0, SEEK_END)) {
    throwFileError(FileErrorKind::InvalidArgument, "The ar archive has a truncated trailing header.");
  }
}
