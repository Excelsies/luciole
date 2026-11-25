#include "operations/archive_extra_formats.h"

#include "fs/archive_extra_types.h"

#include <archive.h>

void luciole::archive_work::enableExtraFilters(struct archive* reader, Work& work) {
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Lz4)) {
    requireArchive(archive_read_support_filter_lz4(reader), reader, work);
  }
  if (archiveExtraNeedAvailable(ArchiveExtraNeed::Compress)) {
    requireArchive(archive_read_support_filter_compress(reader), reader, work);
  }
}

bool luciole::archive_work::extraFilterNeedsRawPass(int filterCode) {
  return filterCode == ARCHIVE_FILTER_LZ4 || filterCode == ARCHIVE_FILTER_COMPRESS;
}
