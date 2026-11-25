#include "archive_rar_fixtures.h"
#include "archive_sevenzip_fixtures.h"
#include "event_loop/glib_poll_source.h"
#include "fs/archive_container.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

// Every archive container type, as a small file of its real format with the entries that family starts with,
// browsed and unpacked through the real archive service under the type's own file name.
namespace {
  namespace fs = std::filesystem;
  using luciole::ArchiveContainerFormat;
  using luciole::ArchiveFamily;
  using namespace std::chrono_literals;

  struct Folder {
    luciole::test::TempFolder tempFolder{"luciole-archive-container-test", "/tmp"};
    fs::path root = tempFolder.path();
    Folder() {
      fs::create_directory(root / "tmp");
      ::setenv("TMPDIR", (root / "tmp").c_str(), 1);
    }
  };

  void waitFor(luciole::ArchiveService& service) {
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (service.busy()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      auto start = source.addPollFds(fds);
      const int timeout = source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
      source.dispatch(fds, start);
    }
  }
  std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void write(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(out.good());
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }

  struct Member {
    std::string name, data;
  };
  constexpr std::string_view kPayload = "payload bytes of the container\n";

  // The entries a family's files start with, then a payload every file carries.
  std::vector<Member> membersOf(const luciole::ArchiveContainerType& type) {
    std::vector<Member> members;
    const std::string name(type.name);
    if (name == "epub") {
      members = {{"mimetype", "application/epub+zip"}, {"META-INF/container.xml", "<container/>"}};
    } else if (name == "ora") {
      members = {{"mimetype", "image/openraster"}, {"stack.xml", "<image/>"}};
    } else if (name == "kra") {
      members = {{"mimetype", "application/x-krita"}, {"maindoc.xml", "<DOC/>"}};
    } else if (type.family == ArchiveFamily::OpenDocument) {
      members = {{"mimetype", std::string(type.mime)}, {"META-INF/manifest.xml", "<manifest/>"}};
    } else if (type.family == ArchiveFamily::Java) {
      members = {{"META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n"}, {"app/Main.class", "class"}};
    } else if (type.family == ArchiveFamily::Android) {
      members = {{"AndroidManifest.xml", "<manifest/>"}, {"classes.dex", "dex"}};
    } else if (type.family == ArchiveFamily::OfficeOpenXml || name == "appx" || name == "msix" || name == "3mf") {
      members = {{"[Content_Types].xml", "<Types/>"}, {"_rels/.rels", "<Relationships/>"}};
    } else if (name == "cbz") {
      members = {{"001.png", "png"}, {"002.png", "png"}};
    } else if (name == "whl") {
      members = {{"pkg/__init__.py", ""}, {"pkg-1.0.dist-info/WHEEL", "Wheel-Version: 1.0\n"}};
    } else if (name == "crate") {
      members = {{"demo-1.0.0/Cargo.toml", "[package]\n"}};
    } else if (name == "gem") {
      members = {{"metadata.gz", "meta"}, {"data.tar.gz", "data"}};
    } else {
      members = {{"manifest", "{}"}};
    }
    members.push_back({"payload.txt", std::string(kPayload)});
    return members;
  }

  // Writes a ZIP, TAR or gzip-compressed TAR with libarchive; ODF-style files keep every entry stored.
  void generate(const fs::path& path, const luciole::ArchiveContainerType& type) {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    if (type.format == ArchiveContainerFormat::Zip) {
      TEST_CHECK(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_zip_set_compression_store(writer.get()) == ARCHIVE_OK);
    } else {
      TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(
          archive_write_add_filter(
              writer.get(), type.format == ArchiveContainerFormat::TarGzip ? ARCHIVE_FILTER_GZIP : ARCHIVE_FILTER_NONE
          )
          == ARCHIVE_OK
      );
    }
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& member : membersOf(type)) {
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), member.name.c_str());
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), static_cast<la_int64_t>(member.data.size()));
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      if (!member.data.empty()) {
        TEST_CHECK(
            archive_write_data(writer.get(), member.data.data(), member.data.size())
            == static_cast<la_ssize_t>(member.data.size())
        );
      }
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }

  void browseAndUnpack(const Folder& folder, const luciole::ArchiveContainerType& type) {
    const auto source = folder.root / ("sample." + std::string(type.name));
    generate(source, type);
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    waitFor(service);
    if (service.error()) {
      std::println(stderr, "{}: {}", type.name, service.error()->message);
    }
    const auto catalog = service.catalog();
    TEST_CHECK(!service.error() && catalog && !catalog->encrypted);
    if (!catalog) {
      return;
    }
    for (const auto& member : membersOf(type)) {
      TEST_CHECK(std::ranges::any_of(catalog->entries, [&](const auto& entry) { return entry.path == member.name; }));
    }
    const auto output = folder.root / ("out-" + std::string(type.name));
    TEST_CHECK(service.extract(location(source), location(output), {"payload.txt"}, catalog));
    waitFor(service);
    TEST_CHECK(!service.error() && read(output / "payload.txt") == kPayload);
    TEST_CHECK(luciole::isBrowsableArchive(location(source), std::string(type.mime)));
  }

  // The 7-Zip and RAR files come from the fixtures the archive service test uses.
  void sevenZipContainer(const Folder& folder) {
    const auto* fixture = std::ranges::find(sevenzip_fixtures::archives, "lzma2", &sevenzip_fixtures::Fixture::name);
    TEST_CHECK(fixture != sevenzip_fixtures::archives.end());
    gsize size = 0;
    const std::unique_ptr<guchar, decltype(&g_free)> bytes(g_base64_decode(fixture->base64.data(), &size), &g_free);
    TEST_CHECK(bytes && size > 32);
    const auto source = folder.root / "sample.cb7";
    write(source, {reinterpret_cast<const char*>(bytes.get()), size});
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    waitFor(service);
    const auto catalog = service.catalog();
    TEST_CHECK(!service.error() && catalog && catalog->format == "7-Zip" && catalog->entries.size() == 3);
    const auto output = folder.root / "out-cb7";
    TEST_CHECK(service.extract(location(source), location(output), {"second.txt"}, catalog));
    waitFor(service);
    std::string expected;
    for (unsigned i = 0; i < 32; ++i) {
      expected += "second payload\n";
    }
    TEST_CHECK(!service.error() && read(output / "second.txt") == expected);
  }
  void rarContainer(const Folder& folder) {
    const auto& reference = rar_fixtures::archives.front();
    gsize size = 0;
    const std::unique_ptr<guchar, decltype(&g_free)> bytes(g_base64_decode(reference.base64.data(), &size), &g_free);
    TEST_CHECK(bytes && size > 32);
    const auto source = folder.root / "sample.cbr";
    write(source, {reinterpret_cast<const char*>(bytes.get()), size});
    luciole::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    waitFor(service);
    const auto catalog = service.catalog();
    TEST_CHECK(!service.error() && catalog && catalog->format.starts_with("RAR") && !catalog->encrypted);
    const auto output = folder.root / "out-cbr";
    TEST_CHECK(service.extract(location(source), location(output), {std::string(reference.member)}, catalog));
    waitFor(service);
    TEST_CHECK(!service.error() && fs::file_size(output / reference.member) == reference.size);
  }
} // namespace

int main() {
  const Folder folder;
  for (const auto& type : luciole::kArchiveContainerTypes) {
    if (type.format == ArchiveContainerFormat::SevenZip) {
      sevenZipContainer(folder);
    } else if (type.format == ArchiveContainerFormat::Rar) {
      rarContainer(folder);
    } else {
      browseAndUnpack(folder, type);
    }
  }
  std::println("archive-container-service: all checks passed");
  return 0;
}
