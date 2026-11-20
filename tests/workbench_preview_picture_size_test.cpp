#include "animation_files.h"
#include "fs/gio_ptr.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"
#include "workbench_place_rig.h"

#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The facts line of a picture's preview states the file's own pixel size, whatever the box the picture is decoded for:
// in a column, in the Jump ahead pane, and when the picture comes from the thumbnail cache.
namespace {
  using namespace luciole::test::place;

  struct Picture {
    std::string name;
    int width;
    int height;
  };

  // Frames until the facts line names a pixel size, and returns that size as the line writes it.
  std::string pixelSize(Rig& rig, const std::string& name) {
    rig.select(name);
    rig.frame();
    rig.skew += luciole::WorkbenchPreview::kDwell;
    std::string found;
    rig.frames([&] {
      const std::string facts(rig.slot().factsLabel().text());
      const auto at = facts.find(" × ");
      if (rig.slot().nameLabel().text() != name || at == std::string::npos) {
        return false;
      }
      const auto start = facts.rfind(" · ", at);
      found = facts.substr(start == std::string::npos ? 0 : start + 4);
      return true;
    });
    return found;
  }

  void
  everyHomeStatesTheFilesOwnSize(::test::SceneCapture& capture, const luciole::test::preview::PreviewFiles& files) {
    const std::vector<Picture> pictures{{"pic800.png", 800, 500}, {"pic3000.png", 3000, 2000}};
    for (const auto& picture : pictures) {
      TEST_CHECK(
          ::test::writeBytes(
              files.folder() / picture.name, ::test::stillPng(picture.width, picture.height, {9, 99, 199, 255})
          )
      );
    }
    Rig rig(files, capture);
    for (const auto size : {kSmall, kWide, kSmall}) { // the last visit finds the pictures in the thumbnail cache
      rig.size = size;
      rig.choice = size.width == kSmall.width ? WorkbenchPreviewPlace::JumpAheadWhenNarrow
                                              : WorkbenchPreviewPlace::HiddenWhenNarrow;
      for (const auto& picture : pictures) {
        const auto want = std::to_string(picture.width) + " × " + std::to_string(picture.height);
        std::println("{} at {} wide reads {}", picture.name, size.width, pixelSize(rig, picture.name));
        TEST_CHECK(pixelSize(rig, picture.name) == want);
        rig.select("notes.md"); // another file between visits
        rig.frame();
      }
    }
  }

  std::vector<std::uint8_t> workerResult(std::string_view magic, std::vector<std::uint32_t> sizes) {
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    for (const auto size : sizes) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(size >> shift));
      }
    }
    bytes.resize(bytes.size() + (static_cast<std::size_t>(sizes[0]) * sizes[1] * 4), 7);
    return bytes;
  }

  // A worker's result carries the file's own size beside the decoded one; one that states half of it, or an absurd
  // one, is refused; the plain form states none.
  void theWorkerResultCarriesTheFilesOwnSize() {
    const auto sized = luciole::readThumbnailWorkerResult(workerResult("NFR2", {4, 2, 800, 500}), 128);
    TEST_CHECK(
        sized && sized->width == 4 && sized->height == 2 && sized->sourceWidth == 800 && sized->sourceHeight == 500
    );
    const auto unsized = luciole::readThumbnailWorkerResult(workerResult("NFR2", {4, 2, 0, 0}), 128);
    TEST_CHECK(unsized && unsized->sourceWidth == 0 && unsized->sourceHeight == 0);
    TEST_CHECK(!luciole::readThumbnailWorkerResult(workerResult("NFR2", {4, 2, 800, 0}), 128));
    TEST_CHECK(!luciole::readThumbnailWorkerResult(workerResult("NFR2", {4, 2, 5'000'000, 500}), 128));
    const auto plain = luciole::readThumbnailWorkerResult(workerResult("NFR1", {4, 2}), 128);
    TEST_CHECK(plain && plain->sourceWidth == 0 && plain->width == 4);
  }

  // A cached thumbnail keeps the size of the file it was made from, and gives it back at the cached size and below.
  void theCacheKeepsTheFilesOwnSize() {
    luciole::ThumbnailStore store;
    const luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto uri = luciole::Location::fromPath("/tmp/own-size.png").uri();
    const auto plain = luciole::Location::fromPath("/tmp/own-size-plain.png").uri();
    const auto make = [](const std::string& key, bool sized) {
      luciole::ThumbnailPng thumbnail{
          .image = {.rgba = std::vector<std::uint8_t>(512U * 320U * 4U, 200), .width = 512, .height = 320},
          .text = {{"Thumb::URI", key}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}, {"X-Luciole::Stamp", "same"}}
      };
      if (sized) {
        thumbnail.text["Thumb::Image::Width"] = "800";
        thumbnail.text["Thumb::Image::Height"] = "500";
      }
      return thumbnail;
    };
    store.write(make(uri, true), 512, 16U * 1024U * 1024U, cancel.get());
    store.write(make(plain, false), 512, 16U * 1024U * 1024U, cancel.get());
    for (const int side : {512, 256}) {
      const auto cached = store.read(uri, side, 123, 42, "same");
      TEST_CHECK(cached && cached->image.sourceWidth == 800 && cached->image.sourceHeight == 500);
    }
    const auto unsized = store.read(plain, 512, 123, 42, "same");
    TEST_CHECK(unsized && unsized->image.sourceWidth == 0 && unsized->image.sourceHeight == 0);
    TEST_CHECK(store.clear().empty());
  }
} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return luciole::runThumbnailWorker(std::span(args).subspan(1));
  }
  if (!args.empty()) {
    return 2;
  }
  std::string reason;
  const auto capture = ::test::SceneCapture::create(reason);
  if (!capture) {
    std::println("skipped: {}", reason);
    return ::test::kSkipExitCode;
  }
  static const luciole::test::preview::PreviewFiles files;
  theWorkerResultCarriesTheFilesOwnSize();
  theCacheKeepsTheFilesOwnSize();
  everyHomeStatesTheFilesOwnSize(*capture, files);
  std::println("workbench-preview-picture-size: all checks passed");
  return 0;
}
