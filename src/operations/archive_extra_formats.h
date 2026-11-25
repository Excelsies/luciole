#pragma once

#include "operations/archive_support.h"

struct archive;

// Reading the archive types beyond ZIP, TAR, 7-Zip and RAR: lz4 and compress filters. LZMA-alone and lzip decode
// outside libarchive, in XzInput. Every filter is enabled only when libarchive decodes it in process, so libarchive
// never starts a helper program.
namespace luciole::archive_work {
  void enableExtraFilters(struct archive* reader, Work& work);
  // Whether a stream behind this filter can end its framing after the TAR does, so only a pass over all of it checks.
  bool extraFilterNeedsRawPass(int filterCode);
} // namespace luciole::archive_work
