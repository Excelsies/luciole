#include "bink2_worker.h"
#include "fs/gio_ptr.h"
#include "media/bink2_poster.h"
#include "media/media_player.h"
#include "temp_folder.h"
#include "thumbnails/thumbnail_provider.h"
#include "thumbnails/thumbnail_service.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <print>
#include <vector>

extern "C" {
#include <bink2.h>
}

// The poster of a Bink 2 file, which the thumbnail path makes with the bundled decoder. With a real file
// (LUCIOLE_BINK2_SAMPLE; without it those checks exit with meson's skip code, 77) the picture is checked against the
// library's own RGB conversion of the first frames; the checks that need no file refuse what is not Bink 2, headers
// that cannot be played and damaged frames, and run first.
namespace {
  using namespace test::bink2;
  using luciole::media::bink2Poster;
  using Clock = std::chrono::steady_clock;

  int openFile(const fs::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    TEST_CHECK(fd >= 0);
    return fd;
  }

  void write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  // A system without MIME globs for bik and bk2 types them as plain bytes; the app's name table still says video.
  void namedVideosGoToThePosterPath() {
    luciole::DirectoryEntry entry;
    entry.name = "intro.bk2";
    entry.contentType = "application/octet-stream";
    TEST_CHECK(luciole::thumbnailContentType(entry).value_or("").starts_with("video/"));
    entry.name = "INTRO.BIK";
    entry.contentType = nullptr;
    TEST_CHECK(luciole::thumbnailContentType(entry).value_or("").starts_with("video/"));
    // A known type wins, and other names stay as they are.
    entry.contentType = "text/plain";
    TEST_CHECK(luciole::thumbnailContentType(entry) == luciole::InternedText("text/plain"));
    entry.name = "notes.dat";
    entry.contentType = "application/octet-stream";
    TEST_CHECK(luciole::thumbnailContentType(entry) == luciole::InternedText("application/octet-stream"));
    entry.name = "intro.bik";
    entry.contentType = "video/vnd.radgamettools.bink";
    TEST_CHECK(luciole::thumbnailContentType(entry) == luciole::InternedText("video/vnd.radgamettools.bink"));
  }

  // What the helper says of a file it cannot make a picture of.
  std::string refusal(const fs::path& folder, const fs::path& path) {
    const auto result = runWorker(folder, path, 256);
    TEST_CHECK(!result.started && !result.failure.empty());
    return result.failure;
  }

  void refuses(const fs::path& folder) {
    write(folder / "text.bik", std::vector<std::uint8_t>(300, 'a'));
    TEST_CHECK(refusal(folder, folder / "text.bik").contains("not a Bink 2"));
    Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0xFF)};
    write(folder / "damaged.bik", container.bytes());
    const auto reason = refusal(folder, folder / "damaged.bik");
    TEST_CHECK(reason.contains("Frame 0"));
    container.width = luciole::kMaxMediaFrameSide + 1;
    write(folder / "wide.bik", container.bytes());
    TEST_CHECK(refusal(folder, folder / "wide.bik").contains("at most 4096"));
    container.width = 64;
    container.flags |= 0x00100000;
    write(folder / "alpha.bik", container.bytes());
    TEST_CHECK(refusal(folder, folder / "alpha.bik").contains("transparency"));
    // Through the thumbnail call, which starts the helper itself: the reason comes back and there is no picture.
    const int fd = openFile(folder / "damaged.bik");
    luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto thumbnail = luciole::renderThumbnailProvider(fd, "video/vnd.radgamettools.bink", 128, {}, cancel.get());
    ::close(fd);
    TEST_CHECK(!thumbnail && thumbnail.error().contains("Frame 0"));
    // Only a file that starts with the signature goes to the helper: other video goes to the FFmpeg path.
    const int other = openFile(folder / "text.bik");
    const auto notBink = luciole::renderThumbnailProvider(other, "video/mp4", 128, {}, cancel.get());
    ::close(other);
    TEST_CHECK(!notBink && !notBink.error().contains("not a Bink 2"));
    std::println("a damaged file: {}", thumbnail.error());
  }

  // Mean red, green and blue of the library's RGB for each of the first frames.
  std::vector<std::array<double, 3>> libraryMeans(const FileReader& file, int frames) {
    auto read = file.reader();
    const auto callback = [](void* user, unsigned long offset, void* destination, unsigned long bytes) {
      return (*static_cast<luciole::media::bink2::Stream::Read*>(user))(offset, destination, bytes) ? 0 : -1;
    };
    bink2 library = bink2_open_callbacks(callback, &read, static_cast<unsigned long>(file.size()));
    TEST_CHECK(library != nullptr && bink2_enable_video(library, 1) == 0);
    unsigned long width = 0;
    unsigned long height = 0;
    TEST_CHECK(bink2_info_video(library, &width, &height, nullptr) == 0);
    std::vector<std::array<double, 3>> means;
    for (int frame = 0; frame < frames; ++frame) {
      TEST_CHECK((frame == 0 ? bink2_first(library) : bink2_next(library)) >= 0);
      const unsigned char* rgb = bink2_get_video(library);
      TEST_CHECK(rgb != nullptr);
      std::array<std::uint64_t, 3> sum{};
      for (std::size_t i = 0; i < width * height; ++i) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
          sum[channel] += rgb[i * 3 + channel];
        }
      }
      means.push_back(
          {static_cast<double>(sum[0]) / static_cast<double>(width * height),
           static_cast<double>(sum[1]) / static_cast<double>(width * height),
           static_cast<double>(sum[2]) / static_cast<double>(width * height)}
      );
    }
    bink2_close(library);
    return means;
  }

  void posterOfTheSample(const fs::path& folder, const fs::path& path) {
    FileReader file(path);
    const int fd = openFile(path);
    const auto begin = Clock::now();
    const auto made = runWorker(folder, path, 256);
    const double took = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    if (!made.started) {
      std::println(stderr, "helper: {}", made.failure);
    }
    TEST_CHECK(made.started);
    const auto poster = bink2Poster(fd, 256, [] { return false; });
    TEST_CHECK(poster.has_value());
    // What the helper writes is what the decoder makes in this process, byte for byte.
    TEST_CHECK(made.image.width == poster->width && made.image.height == poster->height);
    TEST_CHECK(made.image.rgba == poster->rgba);
    auto opened = luciole::media::bink2::Stream::open(file.reader(), file.size());
    TEST_CHECK(opened.has_value());
    const auto& info = (*opened)->info();
    // Fitted into 256 by 256, the aspect kept, never enlarged.
    TEST_CHECK(std::max(poster->width, poster->height) <= 256 && std::max(poster->width, poster->height) >= 255);
    TEST_NEAR(static_cast<double>(poster->width) / poster->height, static_cast<double>(info.width) / info.height, 0.02);
    TEST_CHECK(poster->rgba.size() == static_cast<std::size_t>(poster->width) * poster->height * 4);
    std::array<double, 3> mean{};
    for (std::size_t i = 0; i < poster->rgba.size(); i += 4) {
      TEST_CHECK(poster->rgba[i + 3] == 255);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        mean[channel] += poster->rgba[i + channel];
      }
    }
    for (auto& value : mean) {
      value /= static_cast<double>(poster->rgba.size() / 4);
    }
    // The picture is one of the first frames: its mean color is that of one of them.
    const auto frames = static_cast<int>(std::min<std::uint64_t>(info.frames, luciole::media::kPosterFrames));
    const auto means = libraryMeans(file, frames);
    const bool found = std::ranges::any_of(means, [&](const auto& library) {
      return std::abs(library[0] - mean[0]) < 3
          && std::abs(library[1] - mean[1]) < 3
          && std::abs(library[2] - mean[2]) < 3;
    });
    TEST_CHECK(found);
    // The thumbnail call gives the same picture, as a preview and as a list thumbnail.
    luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
    for (const auto purpose : {luciole::ThumbnailPurpose::Preview, luciole::ThumbnailPurpose::Thumbnail}) {
      const auto image =
          luciole::renderThumbnailProvider(fd, "video/vnd.radgamettools.bink", 256, {}, cancel.get(), purpose);
      TEST_CHECK(image && image->width == poster->width && image->height == poster->height);
      TEST_CHECK(image->rgba == poster->rgba);
    }
    // A cancelled request ends without a picture.
    g_cancellable_cancel(cancel.get());
    const auto cancelled = luciole::renderThumbnailProvider(
        fd, "video/vnd.radgamettools.bink", 256, {}, cancel.get(), luciole::ThumbnailPurpose::Preview
    );
    TEST_CHECK(!cancelled && cancelled.error().contains("cancelled"));
    ::close(fd);
    std::println(
        "poster {}x{} from a {}x{} file in {:.0f} ms", poster->width, poster->height, info.width, info.height, took
    );
  }
} // namespace

int main(int argc, char** argv) {
  if (const int code = dispatchBink2Worker(argc, argv); code >= 0) {
    return code;
  }
  const luciole::test::TempFolder folder{"luciole-bink2-poster"};
  refuses(folder.path());
  namedVideosGoToThePosterPath();
  const auto sample = sampleFrom(kSampleVariable);
  posterOfTheSample(folder.path(), needSample(sample, kSampleVariable));
  std::println("bink2 poster checks passed");
  return 0;
}
