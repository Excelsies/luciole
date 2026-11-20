#include "bink2_worker.h"
#include "fs/gio_interop.h"
#include "media/media_player.h"
#include "temp_folder.h"
#include "thumbnails/metadata_probe.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <print>

// The details rows of a Bink 2 file in the quick preview and the docked pane: the frame size, duration, frame rate,
// the codec and the sound track, read from the header by a worker of the application, the way the other videos' rows
// are read by a worker; a header that fails the playback checks gives only the rows an unreadable video gets. The
// header is built by hand, so it needs no file; with LUCIOLE_BINK2_SAMPLE the rows of a real file are compared with
// what its header says, and without it that part exits with meson's skip code, 77.
namespace {
  namespace bink = test::bink2;
  using namespace test::bink2;

  luciole::DirectoryEntry entryOf(const fs::path& path, const std::string& mime) {
    const auto location = luciole::Location::fromPath(path.native());
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location).get(),
        "standard::size,time::modified,id::file,id::filesystem,etag::value,time::changed,time::changed-nsec,time::"
        "changed-usec",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info != nullptr);
    luciole::DirectoryEntry result{
        .id = 1,
        .place = location,
        .name = path.filename().native(),
        .type = luciole::FileType::Regular,
        .size = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE),
        .modifiedSeconds = g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED),
        .contentType = mime,
        .contentRevision = luciole::readChangeRevision(info.get()),
    };
    result.setEtag(g_file_info_get_etag(info.get()));
    result.setIdentity(luciole::readFileIdentity(info.get()));
    return result;
  }

  std::string value(const luciole::MetadataFields& fields, const std::string& key) {
    const auto found = std::ranges::find_if(fields, [&](const auto& field) { return field.label == key; });
    return found != fields.end() ? found->value : std::string{};
  }

  luciole::MetadataFields rows(const fs::path& path) {
    const luciole::GioPtr<GCancellable> cancel(g_cancellable_new());
    return luciole::probePreviewMetadata(entryOf(path, "video/vnd.radgamettools.bink"), {}, cancel.get());
  }

  void write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  void rowsOfAHandMadeHeader(const fs::path& folder) {
    Container container;
    container.width = 640;
    container.height = 360;
    container.rateNumerator = 30000;
    container.rateDenominator = 1001;
    container.frames = std::vector(60, std::vector<std::uint8_t>(16, 0));
    container.tracks = 1;
    container.maxDecoded = 145920;
    write(folder / "made.bik", container.bytes());
    const auto fields = rows(folder / "made.bik");
    TEST_CHECK(value(fields, "Container") == "Bink 2");
    TEST_CHECK(value(fields, "Video codec") == "Bink 2");
    TEST_CHECK(value(fields, "Frame size") == "640 × 360 px");
    TEST_CHECK(value(fields, "Frame rate") == "29.970 fps");
    TEST_CHECK(value(fields, "Duration") == "0:00:02.002");
    TEST_CHECK(value(fields, "Audio codec") == "Bink Audio 2 (DCT)");
    TEST_CHECK(value(fields, "Sample rate") == "48000 Hz");
    TEST_CHECK(value(fields, "Channels") == "2");
    TEST_CHECK(std::ranges::any_of(fields, [](const auto& field) {
      return field.label == "Frame size" && field.preview;
    }));
  }

  // A header that cannot be played gives the rows of an unreadable video: those the file system gives, no video rows.
  void rowsOfAHeaderThatCannotBePlayed(const fs::path& folder) {
    Container container;
    container.frames = {std::vector<std::uint8_t>(64, 0)};
    container.width = luciole::kMaxMediaFrameSide + 1;
    write(folder / "wide.bik", container.bytes());
    auto fields = rows(folder / "wide.bik");
    TEST_CHECK(!fields.empty() && value(fields, "Video codec").empty() && value(fields, "Frame size").empty());
    container.width = 64;
    container.flags |= 0x00100000;
    write(folder / "alpha.bik", container.bytes());
    fields = rows(folder / "alpha.bik");
    TEST_CHECK(!fields.empty() && value(fields, "Video codec").empty());
    container.flags = 2;
    container.claimedFrames = 900'000;
    write(folder / "short.bik", container.bytes());
    TEST_CHECK(value(rows(folder / "short.bik"), "Video codec").empty());
    write(folder / "text.bik", std::vector<std::uint8_t>(200, 'z'));
    TEST_CHECK(value(rows(folder / "text.bik"), "Video codec").empty());
  }

  void rowsOfTheSample(const fs::path& sample) {
    std::ifstream in(sample, std::ios::binary);
    std::vector<std::uint8_t> head(64);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const auto frames = le32(head, 8);
    const double rate = static_cast<double>(le32(head, 28)) / le32(head, 32);
    const auto fields = rows(sample);
    TEST_CHECK(value(fields, "Video codec") == "Bink 2");
    TEST_CHECK(value(fields, "Frame size") == std::format("{} × {} px", le32(head, 20), le32(head, 24)));
    TEST_CHECK(std::abs(std::stod(value(fields, "Frame rate")) - rate) < 0.001);
    TEST_CHECK(!value(fields, "Duration").empty());
    TEST_CHECK(value(fields, "Frames").empty() || std::stoul(value(fields, "Frames")) == frames);
    TEST_CHECK((le32(head, 40) != 0) == !value(fields, "Audio codec").empty());
    std::println(
        "rows of a {} frame file: {} at {} fps, {}", frames, value(fields, "Frame size"), value(fields, "Frame rate"),
        value(fields, "Duration")
    );
  }
} // namespace

int main(int argc, char** argv) {
  if (const int code = dispatchBink2Worker(argc, argv); code >= 0) {
    return code;
  }
  const luciole::test::TempFolder folder{"luciole-bink2-facts"};
  rowsOfAHandMadeHeader(folder.path());
  rowsOfAHeaderThatCannotBePlayed(folder.path());
  const auto sample = sampleFrom(kSampleVariable);
  rowsOfTheSample(needSample(sample, kSampleVariable));
  std::println("bink2 facts checks passed");
  return 0;
}
