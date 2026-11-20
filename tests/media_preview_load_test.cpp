#include "browser/workbench_services.h"
#include "media/media_probe.h"
#include "media_clips.h"
#include "test_check.h"
#include "ui/workbench/column_preview_loads.h"
#include "ui/workbench/column_preview_view.h"

#include <memory>
#include <print>

// A video or sound file in a Workbench column: its facts line gains the playing time and picture size from the
// container, before anything plays.
namespace {
  using luciole::ColumnPreviewKind;
  using luciole::ColumnPreviewLoad;
  using luciole::ColumnPreviewRequest;
  using luciole::ColumnPreviewView;
  using namespace test::media;

  luciole::DirectoryEntry entryFor(const fs::path& path) {
    luciole::DirectoryEntry entry;
    entry.place = luciole::Location::fromPath(path.string());
    entry.name = path.filename().string();
    entry.type = luciole::FileType::Regular;
    return entry;
  }

  struct Loaded {
    ColumnPreviewView view;
    int frames = 0;
    std::unique_ptr<ColumnPreviewLoad> load;

    explicit Loaded(const fs::path& path) {
      const ColumnPreviewRequest request{.entry = entryFor(path), .kind = ColumnPreviewKind::Media};
      load = luciole::startColumnPreviewLoad(view, request, luciole::WorkbenchPreviewWorkers{}, [this] { ++frames; });
      TEST_CHECK(load != nullptr);
    }
  };

  void videoFactsArriveBeforePlay(Pump& pump, const fs::path& clip) {
    Loaded loaded(clip);
    TEST_CHECK(!loaded.load->facts().seconds && !loaded.load->facts().pixels);
    TEST_CHECK(pump.until([&] { return loaded.load->facts().seconds.has_value(); }));
    TEST_NEAR(*loaded.load->facts().seconds, 3.0, 0.05);
    TEST_CHECK(loaded.load->facts().pixels == std::pair(160, 120));
    TEST_CHECK(loaded.frames > 0);
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }

  void soundFactsHaveNoPicture(Pump& pump, const fs::path& tone) {
    Loaded loaded(tone);
    TEST_CHECK(pump.until([&] { return loaded.load->facts().seconds.has_value(); }));
    TEST_NEAR(*loaded.load->facts().seconds, 2.0, 0.05);
    TEST_CHECK(!loaded.load->facts().pixels);
  }

  // A load that goes before its facts arrive asks for no frame, and its file is closed.
  void goesQuietly(Pump& pump, const fs::path& clip) {
    Loaded loaded(clip);
    loaded.load.reset();
    pump.run(std::chrono::milliseconds(500));
    TEST_CHECK(loaded.frames == 0);
    TEST_CHECK(pump.until([&] { return !holdsOpen(clip); }));
  }
} // namespace

int main() {
  if (!luciole::MediaProbe::available()) {
    std::println("skipped: playback not built");
    return ::test::kSkipExitCode;
  }
  Scratch scratch;
  if (const auto missing = missingElements(
          {"videotestsrc", "audiotestsrc", "avimux", "filesink", "capsfilter", "avidemux", "wavparse", "parsebin",
           "fakesink"}
      );
      !missing.empty()) {
    std::println("skipped: GStreamer lacks {}", missing);
    return ::test::kSkipExitCode;
  }
  const auto clip = scratch.path("clip.avi");
  const auto tone = scratch.path("tone.wav");
  TEST_CHECK(writeClip(clip, {}));
  writeTone(tone, 2);
  Pump pump;
  videoFactsArriveBeforePlay(pump, clip);
  soundFactsHaveNoPicture(pump, tone);
  goesQuietly(pump, clip);
  std::println("media-preview-load: all checks passed");
  return 0;
}
