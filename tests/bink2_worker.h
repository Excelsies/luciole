#pragma once

#include "bink2_sample.h"
#include "fs/gio_ptr.h"
#include "media/bink2_poster.h"
#include "render/core/image_decoder.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <fcntl.h>
#include <span>
#include <string>
#include <string_view>

// The Bink 2 poster helper run the way the thumbnail path runs it: as a mode of this test program, which the
// provider starts as "/proc/self/exe --bink2-poster-worker", with the real limits of runProviderProcess. A test
// program that uses it calls dispatchBink2Worker first in main.
namespace test::bink2 {
  // Runs the helper when the program was started as one; otherwise returns -1.
  inline int dispatchBink2Worker(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bink2-poster-worker") {
      std::vector<std::string_view> arguments(argv + 2, argv + argc);
      return luciole::media::runBink2PosterWorker(arguments);
    }
    if (argc > 1 && std::string_view(argv[1]) == "--bink2-facts-worker") {
      std::vector<std::string_view> arguments(argv + 2, argv + argc);
      return luciole::media::runBink2FactsWorker(arguments);
    }
    return -1;
  }

  struct WorkerResult {
    bool started = false;  // the provider ran the child to a normal end
    bool reported = false; // the helper wrote a result file, a picture or a reason
    std::string failure;   // why not, or the reason the helper wrote
    LoadedImageFile image;
  };

  // The helper on `path`, asked for a picture fitted into `side`, in a private folder of its own.
  inline WorkerResult runWorker(const fs::path& folder, const fs::path& path, int side) {
    const fs::path output = folder / "poster.result";
    std::error_code ignored;
    fs::remove(output, ignored);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    TEST_CHECK(fd >= 0);
    luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto ran = luciole::runProviderProcess(
        {"/proc/self/exe", "--bink2-poster-worker", std::to_string(side), output.native()}, fd, cancel.get(),
        8U * 1024U * 1024U, 2
    );
    ::close(fd);
    WorkerResult result;
    auto bytes = luciole::readThumbnailFile(output, 8U * 1024U * 1024U);
    if (!bytes) {
      result.failure = ran ? "no result file" : ran.error();
      return result;
    }
    result.reported = true;
    auto image = luciole::readThumbnailWorkerResult(std::move(*bytes), side);
    if (!image) {
      result.failure = image.error();
      return result;
    }
    result.started = ran.has_value();
    result.image = std::move(*image);
    return result;
  }
} // namespace test::bink2
