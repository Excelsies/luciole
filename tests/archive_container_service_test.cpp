#include "archive_rar_fixtures.h"
#include "archive_sevenzip_fixtures.h"
#include "event_loop/glib_poll_source.h"
#include "fs/archive_container.h"
#include "fs/archive_type.h"
#include "fs/file_error.h"
#include "fs/gio_ptr.h"
#include "fs/native_listing.h"
#include "operations/archive_service.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
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

  std::string decoded(std::string_view base64) {
    gsize size = 0;
    const std::unique_ptr<guchar, decltype(&g_free)> bytes(g_base64_decode(base64.data(), &size), &g_free);
    TEST_CHECK(bytes && size > 32);
    return bytes ? std::string(reinterpret_cast<const char*>(bytes.get()), size) : std::string();
  }
  // The 7-Zip and RAR files come from the fixtures the archive service test uses.
  std::string sevenZipBytes() {
    const auto* fixture = std::ranges::find(sevenzip_fixtures::archives, "lzma2", &sevenzip_fixtures::Fixture::name);
    TEST_CHECK(fixture != sevenzip_fixtures::archives.end());
    return fixture != sevenzip_fixtures::archives.end() ? decoded(fixture->base64) : std::string();
  }
  std::string rarBytes() { return decoded(rar_fixtures::archives.front().base64); }

  void sevenZipContainer(const Folder& folder) {
    const auto source = folder.root / "sample.cb7";
    write(source, sevenZipBytes());
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
    const auto source = folder.root / "sample.cbr";
    write(source, rarBytes());
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

  // Extract Here of a container lands beside it in a folder without the suffix, with no conflict.
  void extractHereBesideTheFile(const Folder& folder) {
    const auto* type = &luciole::kArchiveContainerTypes[*luciole::archiveContainerByName("jar")];
    const auto source = folder.root / "app.jar";
    generate(source, *type);
    const auto target = location(source).parent()->child(luciole::archiveExtractFolderName(location(source)));
    TEST_CHECK(target.nativePath() && fs::path(*target.nativePath()) == folder.root / "app");
    luciole::ArchiveService service;
    TEST_CHECK(service.extract(location(source), target, {}, nullptr));
    waitFor(service);
    TEST_CHECK(!service.error() && read(folder.root / "app" / "payload.txt") == kPayload);
    TEST_CHECK(fs::is_regular_file(source));
  }

  // The content type the listing reports for a local file: the name's guess, and GIO's own answer where the name does
  // not decide.
  std::map<std::string, std::string> reportedTypes(const fs::path& directory) {
    const auto listing = luciole::listNativeFolder(directory.string(), 1000, nullptr);
    TEST_CHECK(!listing.unsupported && !listing.limited);
    std::map<std::string, std::string> types;
    for (std::size_t index = 0; index < listing.entries.size(); ++index) {
      const auto& entry = listing.entries[index];
      std::string type = entry.contentType ? entry.contentType->c_str() : "";
      if (std::ranges::contains(listing.uncertain, static_cast<std::uint32_t>(index))) {
        type = luciole::sniffContentType((directory / entry.name).string()).value_or("");
      }
      types[entry.name] = type;
    }
    return types;
  }

  // browsesByDefault is the table's own claim, so the system's answer cannot change it. A type that has a content type
  // of its own is never on by default. Where the system reports a plain archive for a type, the table must say it
  // browses; any other report is a different MIME database, printed and not failed.
  void theDefaultMatchesWhatTheSystemReports(const Folder& folder) {
    const auto directory = folder.root / "reported";
    fs::create_directory(directory);
    for (const auto& type : luciole::kArchiveContainerTypes) {
      const auto path = directory / ("sample." + std::string(type.name));
      if (type.format == ArchiveContainerFormat::SevenZip) {
        write(path, sevenZipBytes());
      } else if (type.format == ArchiveContainerFormat::Rar) {
        write(path, rarBytes());
      } else {
        generate(path, type);
      }
    }
    const auto reported = reportedTypes(directory);
    TEST_CHECK(reported.size() == luciole::kArchiveContainerCount);
    for (const auto& type : luciole::kArchiveContainerTypes) {
      TEST_CHECK(!type.browsesByDefault || type.mime.empty());
      const auto name = "sample." + std::string(type.name);
      const auto found = reported.find(name);
      TEST_CHECK(found != reported.end());
      if (found == reported.end()) {
        continue;
      }
      const bool plain = luciole::isSupportedArchive(location(directory / name), found->second);
      if (plain == type.browsesByDefault) {
        continue;
      }
      std::println(
          stderr, "{}: this system reports '{}', so Open {} it; the table says browsesByDefault = {}", type.name,
          found->second, plain ? "browses" : "does not browse", type.browsesByDefault
      );
      TEST_CHECK(!plain);
    }
  }

  // A container name on a file that is no archive fails like a broken .zip: an error, no catalog, nothing written.
  void aFileThatIsNoArchiveFails(const Folder& folder, const std::string& suffix, const std::string& body) {
    const auto inspect = [&](const std::string& name) {
      const auto path = folder.root / name;
      write(path, body);
      luciole::ArchiveService service;
      TEST_CHECK(service.inspect(location(path)));
      waitFor(service);
      TEST_CHECK(service.error().has_value() && service.catalog() == nullptr);
      const auto out = folder.root / ("out-" + name);
      TEST_CHECK(service.extract(location(path), location(out), {}, nullptr));
      waitFor(service);
      TEST_CHECK(service.error().has_value() && service.catalog() == nullptr);
      TEST_CHECK(!fs::exists(out) || fs::is_empty(out));
      return service.error().value_or(luciole::FileError{});
    };
    const auto broken = inspect("broken-" + std::to_string(body.size()) + ".zip");
    const auto container = inspect("broken-" + std::to_string(body.size()) + "." + suffix);
    TEST_CHECK(!container.message.empty() && container == broken);
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
  extractHereBesideTheFile(folder);
  theDefaultMatchesWhatTheSystemReports(folder);
  const std::string elf = std::string("\x7f") + "ELF" + std::string(60, '\x01');
  for (const auto* suffix : {"jar", "cbt", "crate"}) {
    for (const auto& body : {std::string(), std::string("plain text, not an archive\n"), elf}) {
      aFileThatIsNoArchiveFails(folder, suffix, body);
    }
  }
  std::println("archive-container-service: all checks passed");
  return 0;
}
