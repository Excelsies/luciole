#pragma once

#include "operations/archive_input.h"
#include "operations/archive_service.h"
#include "operations/archive_support.h"

#include <map>

struct archive;
struct archive_entry;

// Reading the archive types beyond ZIP, TAR, 7-Zip and RAR: lz4 and compress filters, ar and cpio. LZMA-alone and lzip
// decode outside libarchive, in XzInput. Every filter is enabled only when libarchive decodes it in process, so
// libarchive never starts a helper program.
namespace luciole::archive_work {
  void enableExtraFilters(struct archive* reader, Work& work);
  // Registered after the TAR, ZIP and 7-Zip readers, which therefore win a tie of content bids. ar is registered only
  // when the file itself starts as ar and its name tables fit the metadata limit; the input is left at its start.
  void enableExtraFormats(struct archive* reader, Input& input, Work& work);
  // Whether a stream behind this filter can end its framing after the TAR does, so only a pass over all of it checks.
  bool extraFilterNeedsRawPass(int filterCode);
  // The next header; the symbol-table members of ar archives are consumed here and never reach the catalog.
  int nextHeader(struct archive* reader, struct archive_entry** entry, const Work& work);
  // A hard link names data another member holds. One that carries data of its own, which cpio newc does for a file's
  // last link, would extract as an empty file; refuse it unless its target holds the same data.
  void requireHardlinkData(
      struct archive_entry* header, const std::string& target, const std::vector<ArchiveEntry>& entries,
      const std::map<std::string, std::size_t>& known
  );
  // At the end of an ar archive: libarchive treats a partial header there as the end, so a cut file would pass as a
  // shorter one. Fails unless every byte of the file was consumed.
  void requireArchiveEnd(struct archive* reader, Input& input);
} // namespace luciole::archive_work
