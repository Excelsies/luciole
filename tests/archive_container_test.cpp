#include "browser/action_registry.h"
#include "config/settings.h"
#include "fs/archive_container.h"
#include "fs/archive_type.h"
#include "fs/location.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/workbench/column_preview_content.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Archive containers: files that are real archives (ZIP, TAR, 7z, RAR) under another name. The table that names them,
// how a file is recognized, what Open and the menu decide with the setting off and on, and the setting itself.
namespace {
  std::string read(const std::filesystem::path& path);
  using luciole::ArchiveContainerSet;
  using luciole::Location;
  namespace fs = std::filesystem;

  Location file(std::string_view name) { return Location::fromPath("/srv/files/" + std::string(name)); }

  ArchiveContainerSet setOf(std::initializer_list<std::string_view> names) {
    ArchiveContainerSet set;
    for (const auto name : names) {
      const auto index = luciole::archiveContainerByName(name);
      TEST_CHECK(index.has_value());
      set.set(*index);
    }
    return set;
  }

  std::vector<std::string_view> namesOf(const ArchiveContainerSet& set) {
    std::vector<std::string_view> names;
    for (std::size_t index = 0; index < set.size(); ++index) {
      if (set.test(index)) {
        names.push_back(luciole::kArchiveContainerTypes[index].name);
      }
    }
    std::ranges::sort(names);
    return names;
  }

  void theTableHoldsTheListedTypes() {
    const std::vector<std::string_view> listed{
        "jar",  "war",  "ear",   "aar",    "apk",     "aab",     "apks",       "xapk",   "docx", "docm", "dotx",
        "dotm", "xlsx", "xlsm",  "xltx",   "xltm",    "pptx",    "pptm",       "potx",   "potm", "ppsx", "odt",
        "ott",  "ods",  "ots",   "odp",    "otp",     "odg",     "otg",        "odf",    "odb",  "epub", "cbz",
        "xpi",  "vsix", "nupkg", "whl",    "egg",     "oxt",     "ipa",        "appx",   "msix", "kmz",  "3mf",
        "usdz", "ora",  "kra",   "mcpack", "mcworld", "mcaddon", "mctemplate", "mrpack", "pk3",  "pk4",  "love",
        "sb3",  "cbr",  "cb7",   "cbt",    "crate",   "gem"
    };
    TEST_CHECK(luciole::kArchiveContainerCount == listed.size());
    std::set<std::string_view> names;
    std::set<std::string_view> mimes;
    for (const auto& type : luciole::kArchiveContainerTypes) {
      TEST_CHECK(std::ranges::contains(listed, type.name));
      TEST_CHECK(names.insert(type.name).second);
      TEST_CHECK(!type.label.empty() && type.label.contains("(." + std::string(type.name) + ")"));
      TEST_CHECK(type.mime.empty() || mimes.insert(type.mime).second); // a type has its own MIME type
    }
    TEST_CHECK(luciole::archiveFamilyLabel(luciole::ArchiveFamily::Java) == "Java");
    TEST_CHECK(luciole::archiveFamilyLabel(luciole::ArchiveFamily::OfficeOpenXml) == "Office Open XML");
    const auto jar = luciole::archiveContainerByName("jar");
    TEST_CHECK(jar && luciole::kArchiveContainerTypes[*jar].label == "Java Archive (.jar)");
    TEST_CHECK(jar && luciole::kArchiveContainerTypes[*jar].family == luciole::ArchiveFamily::Java);
    TEST_CHECK(
        !luciole::archiveContainerByName("zip")
        && !luciole::archiveContainerByName("JAR")
        && !luciole::archiveContainerByName("")
    );
    // The MIME types the system reports (gio info) for files of these suffixes; the others are suffix-only.
    const auto mimeOf = [](std::string_view name) {
      return std::string(luciole::kArchiveContainerTypes[*luciole::archiveContainerByName(name)].mime);
    };
    TEST_CHECK(mimeOf("jar") == "application/java-archive");
    TEST_CHECK(mimeOf("epub") == "application/epub+zip");
    TEST_CHECK(mimeOf("docx") == "application/vnd.openxmlformats-officedocument.wordprocessingml.document");
    TEST_CHECK(mimeOf("odt") == "application/vnd.oasis.opendocument.text");
    TEST_CHECK(mimeOf("cbr") == "application/vnd.comicbook-rar" && mimeOf("cb7") == "application/x-cb7");
    TEST_CHECK(mimeOf("cbt") == "application/x-cbt");
    // A type the system shares with real archives is suffix-only.
    TEST_CHECK(mimeOf("gem").empty() && mimeOf("crate").empty());
    // Each type names the format it really is.
    const auto formatOf = [](std::string_view name) {
      return luciole::kArchiveContainerTypes[*luciole::archiveContainerByName(name)].format;
    };
    using Format = luciole::ArchiveContainerFormat;
    TEST_CHECK(formatOf("jar") == Format::Zip && formatOf("cbz") == Format::Zip && formatOf("epub") == Format::Zip);
    TEST_CHECK(formatOf("cbr") == Format::Rar && formatOf("cb7") == Format::SevenZip);
    TEST_CHECK(formatOf("cbt") == Format::Tar && formatOf("gem") == Format::Tar);
    TEST_CHECK(formatOf("crate") == Format::TarGzip);
    TEST_CHECK(mimeOf("whl").empty() && mimeOf("war").empty() && mimeOf("nupkg").empty());
  }

  void aGzipContainerNeedsTheGzipFilter() {
    const bool gzip = luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Gzip);
    TEST_CHECK(luciole::isArchiveContainer(file("serde.crate"), "") == gzip);
    TEST_CHECK(luciole::isBrowsableArchive(file("serde.crate"), "") == gzip);
    TEST_CHECK(luciole::opensAsArchive(file("serde.crate"), "", setOf({"crate"})) == gzip);
    TEST_CHECK(luciole::isArchiveContainer(file("rake.gem"), ""));
  }

  // The user documentation lists every type of the table under its label.
  void theDocsListEveryType() {
    const char* source = std::getenv("LUCIOLE_SOURCE_DIR");
    TEST_CHECK(source != nullptr);
    const auto docs = read(fs::path(source) / "docs/user/file-operations.md");
    TEST_CHECK(!docs.empty());
    for (const auto& type : luciole::kArchiveContainerTypes) {
      TEST_CHECK(docs.contains(type.label));
    }
    for (std::size_t family = 0; family < luciole::kArchiveFamilyCount; ++family) {
      const auto label = luciole::archiveFamilyLabel(static_cast<luciole::ArchiveFamily>(family));
      TEST_CHECK(docs.contains("- " + std::string(label) + ": "));
    }
  }

  void recognitionBySuffix() {
    for (const auto& type : luciole::kArchiveContainerTypes) {
      for (const auto& name :
           {"app." + std::string(type.name), "APP." + std::string(type.name) + "",
            "App.Mixed." + std::string(type.name)}) {
        const auto found = luciole::archiveContainerType(file(name), "");
        TEST_CHECK(found && luciole::kArchiveContainerTypes[*found].name == type.name);
      }
    }
    std::string upper = "x.JaR";
    const auto jar = luciole::archiveContainerType(file(upper), "");
    TEST_CHECK(jar == luciole::archiveContainerByName("jar"));
    // Real archives, split parts, other suffixes and a bare suffix are not containers.
    for (const auto* name :
         {"a.zip", "a.tar", "a.tar.gz", "a.7z", "a.rar", "a.zip.001", "a.jar.001", "a.txt", "jar", "a.jarx", "a.ajar",
          "a.jar~"}) {
      TEST_CHECK(!luciole::archiveContainerType(file(name), ""));
      TEST_CHECK(!luciole::isArchiveContainer(file(name), ""));
    }
    TEST_CHECK(!luciole::archiveContainerType(Location::fromUri("trash:///app.jar"), ""));
  }

  void recognitionByMimeType() {
    const auto apk = luciole::archiveContainerByName("apk");
    TEST_CHECK(luciole::archiveContainerType(file("download"), "application/vnd.android.package-archive") == apk);
    TEST_CHECK(
        luciole::archiveContainerType(file("book"), "application/epub+zip") == luciole::archiveContainerByName("epub")
    );
    // The suffix decides first.
    TEST_CHECK(
        luciole::archiveContainerType(file("a.jar"), "application/vnd.android.package-archive")
        == luciole::archiveContainerByName("jar")
    );
    // The shared types are not enough, and neither is a type of another kind of file.
    for (const auto* mime : {"application/zip", "application/x-webarchive", "application/x-tar", "text/plain", ""}) {
      TEST_CHECK(!luciole::archiveContainerType(file("download"), mime));
    }
    TEST_CHECK(!luciole::archiveContainerType(Location::fromUri("trash:///download"), "application/epub+zip"));
  }

  void realArchivesAreNotContainers() {
    for (const auto* name : {"a.zip", "a.tar", "a.tar.gz", "a.7z", "a.rar", "a.zip.001"}) {
      TEST_CHECK(luciole::opensAsArchive(file(name), "", {}) == luciole::isSupportedArchive(file(name), ""));
      TEST_CHECK(luciole::isBrowsableArchive(file(name), "") == luciole::isSupportedArchive(file(name), ""));
    }
    TEST_CHECK(luciole::opensAsArchive(file("a.zip"), "application/zip", {}));
    TEST_CHECK(luciole::opensAsArchive(file("a.zip"), "", setOf({"jar"})));
    TEST_CHECK(!luciole::opensAsArchive(file("a.zip.002"), "", setOf({"jar"})));
    TEST_CHECK(!luciole::opensAsArchive(file("a.txt"), "text/plain", setOf({"jar"})));
    TEST_CHECK(!luciole::isBrowsableArchive(file("a.txt"), "text/plain"));
  }

  // A name that makes a real archive, or a split or volume part, is never a container, whatever MIME type is reported.
  void aRealArchiveNameBeatsTheMimeType() {
    const auto* const openDocument = "application/vnd.oasis.opendocument.text";
    for (const auto* name :
         {"a.zip", "A.ZIP", "a.zip.001", "a.zip.002", "a.tar", "a.tar.gz", "a.tgz", "a.tar.bz2", "a.tar.xz",
          "a.tar.zst", "a.7z", "a.7z.001", "a.rar", "a.part1.rar", "a.r00"}) {
      const bool before = luciole::isSupportedArchive(file(name), "");
      for (const auto* mime : {"application/epub+zip", "application/java-archive", openDocument}) {
        TEST_CHECK(!luciole::archiveContainerType(file(name), mime));
        TEST_CHECK(!luciole::isArchiveContainer(file(name), mime));
        TEST_CHECK(luciole::opensAsArchive(file(name), mime, {}) == before);
        TEST_CHECK(luciole::opensAsArchive(file(name), mime, setOf({"epub", "jar", "odt"})) == before);
      }
    }
    TEST_CHECK(luciole::opensAsArchive(file("a.zip"), "application/epub+zip", {}));
    TEST_CHECK(!luciole::opensAsArchive(file("a.zip.002"), "application/epub+zip", {}));
    // A name that says nothing about the format keeps its MIME type.
    TEST_CHECK(
        luciole::archiveContainerType(file("book"), "application/epub+zip") == luciole::archiveContainerByName("epub")
    );
    TEST_CHECK(
        luciole::archiveContainerType(file("book.dat"), "application/java-archive")
        == luciole::archiveContainerByName("jar")
    );
  }

  // The folder a file extracts into: the file name without its archive suffix, so it never equals the file.
  void theExtractFolderDropsTheContainerSuffix() {
    const auto name = [](std::string_view file_) { return luciole::archiveExtractFolderName(file(file_)); };
    TEST_CHECK(name("app.jar") == "app");
    TEST_CHECK(name("Book.EPUB") == "Book");
    TEST_CHECK(name("Report.Final.docx") == "Report.Final");
    TEST_CHECK(name("x.tar.jar") == "x.tar");
    TEST_CHECK(name("comic.cbr") == "comic" && name("comic.cb7") == "comic" && name("comic.cbt") == "comic");
    TEST_CHECK(name("lib.whl") == "lib" && name("pack.mcpack") == "pack");
    TEST_CHECK(luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Gzip));
    TEST_CHECK(name("serde.crate") == "serde");
    // Nothing left of the name.
    TEST_CHECK(name(".jar") == "Archive" && name(".EPUB") == "Archive");
    // Real archives and other files keep the results of archiveFolderName.
    for (const auto* real : {"plain.zip", "A.TAR.GZ", "b.tgz", "c.7z", "d.rar", "e.tar", "f.zip.001", "g.tar.xz"}) {
      TEST_CHECK(name(real) == luciole::archiveFolderName(file(real)));
    }
    TEST_CHECK(name("plain.zip") == "plain" && name("A.TAR.GZ") == "A" && name("f.zip.001") == "f");
    TEST_CHECK(name("notes.txt") == "notes.txt" && name("download") == "download" && name("a.jarx") == "a.jarx");
    TEST_CHECK(name(".zip") == "Archive");
  }

  void theMenuAlwaysBrowsesAContainer() {
    for (const auto& type : luciole::kArchiveContainerTypes) {
      const auto name = "pack." + std::string(type.name);
      TEST_CHECK(luciole::isBrowsableArchive(file(name), std::string(type.mime)));
      TEST_CHECK(luciole::isBrowsableArchive(file(name), "application/zip"));
    }
    luciole::ArchiveFacts facts{
        .location = file("app.jar"),
        .type = luciole::FileType::Regular,
        .symlink = false,
        .readable = true,
        .mime = "application/java-archive"
    };
    TEST_CHECK(luciole::ActionRegistry::browsableFile(facts));
    TEST_CHECK(luciole::ActionRegistry::browseArchiveState(false, facts).enabled);
    TEST_CHECK(!luciole::ActionRegistry::browseArchiveState(true, facts).enabled);
    facts.readable = false;
    TEST_CHECK(!luciole::ActionRegistry::browseArchiveState(false, facts).enabled);
    facts.readable = true;
    facts.symlink = true;
    TEST_CHECK(!luciole::ActionRegistry::browsableFile(facts));
  }

  void openFollowsTheSetting() {
    const auto jar = setOf({"jar"});
    TEST_CHECK(!luciole::opensAsArchive(file("app.jar"), "application/java-archive", {}));
    TEST_CHECK(luciole::opensAsArchive(file("app.jar"), "application/java-archive", jar));
    TEST_CHECK(luciole::opensAsArchive(file("APP.JAR"), "", jar));
    TEST_CHECK(!luciole::opensAsArchive(file("app.apk"), "", jar));
    TEST_CHECK(luciole::opensAsArchive(file("app.apk"), "", setOf({"apk"})));
    // A type the system reports as a plain ZIP still follows its own setting.
    for (const auto* name : {"lib.whl", "pack.nupkg", "game.love", "x.mcpack"}) {
      TEST_CHECK(!luciole::opensAsArchive(file(name), "application/zip", {}));
      TEST_CHECK(
          luciole::opensAsArchive(
              file(name), "application/zip",
              setOf({std::string_view(name).substr(std::string_view(name).rfind('.') + 1)})
          )
      );
    }
    // By MIME type alone, as a download without a suffix.
    TEST_CHECK(!luciole::opensAsArchive(file("book"), "application/epub+zip", {}));
    TEST_CHECK(luciole::opensAsArchive(file("book"), "application/epub+zip", setOf({"epub"})));
    TEST_CHECK(!luciole::opensAsArchive(Location::fromUri("trash:///app.jar"), "", jar));

    luciole::ArchiveFacts facts{
        .location = file("app.jar"), .type = luciole::FileType::Regular, .symlink = false, .readable = true, .mime = ""
    };
    TEST_CHECK(!luciole::ActionRegistry::opensAsArchive(facts, {}));
    TEST_CHECK(luciole::ActionRegistry::opensAsArchive(facts, jar));
    facts.readable = false;
    TEST_CHECK(!luciole::ActionRegistry::opensAsArchive(facts, jar));
    facts.readable = true;
    facts.type = luciole::FileType::Directory;
    TEST_CHECK(!luciole::ActionRegistry::opensAsArchive(facts, jar));
    luciole::ArchiveFacts zip{
        .location = file("a.zip"), .type = luciole::FileType::Regular, .symlink = false, .readable = true, .mime = ""
    };
    TEST_CHECK(luciole::ActionRegistry::opensAsArchive(zip, {}));
  }

  void thePreviewListsASwitchedOnContainer() {
    using Kind = luciole::ColumnPreviewKind;
    TEST_CHECK(luciole::columnPreviewKind(file("a.zip"), "application/zip", true, true, {}) == Kind::Archive);
    TEST_CHECK(
        luciole::columnPreviewKind(file("app.jar"), "application/java-archive", true, true, {}) != Kind::Archive
    );
    TEST_CHECK(
        luciole::columnPreviewKind(file("app.jar"), "application/java-archive", true, true, setOf({"jar"}))
        == Kind::Archive
    );
    TEST_CHECK(luciole::columnPreviewKind(file("app.apk"), "", true, true, setOf({"jar"})) != Kind::Archive);
    TEST_CHECK(luciole::columnPreviewKind(file("lib.whl"), "application/zip", true, true, {}) != Kind::Archive);
    TEST_CHECK(
        luciole::columnPreviewKind(file("lib.whl"), "application/zip", true, true, setOf({"whl"})) == Kind::Archive
    );
    TEST_CHECK(luciole::columnPreviewKind(file("app.jar"), "", false, true, setOf({"jar"})) != Kind::Archive);
  }

  struct Fixture {
    luciole::test::TempFolder tempFolder{"luciole-archive-container-test"};
    fs::path directory = tempFolder.path();
    void write(const std::string& name, const std::string& text) const {
      std::ofstream out(directory / name, std::ios::binary);
      out << text;
      TEST_CHECK(out.good());
    }
    luciole::ConfigPaths paths() const {
      return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
    }
  };
  std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  void theSettingDefaultsToNone() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload() && service.settings().browser.openAsArchive.none());
    fixture.write("config.toml", "[browser]\nopen_as_archive = []\n");
    TEST_CHECK(service.reload() && service.settings().browser.openAsArchive.none());
  }

  void theSettingReadsTypeNames() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    fixture.write("config.toml", "[browser]\nopen_as_archive = [\"jar\", \"apk\", \"epub\"]\n");
    TEST_CHECK(service.reload());
    TEST_CHECK(
        (namesOf(service.settings().browser.openAsArchive) == std::vector<std::string_view>{"apk", "epub", "jar"})
    );
    fixture.write("config.toml", "[browser]\nopen_as_archive = [\"jar\", \"jar\"]\n");
    TEST_CHECK(
        service.reload() && namesOf(service.settings().browser.openAsArchive) == std::vector<std::string_view>{"jar"}
    );
  }

  void theSettingRejectsWhatItDoesNotKnow() {
    Fixture fixture;
    luciole::ConfigService service(fixture.paths());
    fixture.write("config.toml", "[browser]\nopen_as_archive = [\"jar\"]\n");
    TEST_CHECK(service.reload());
    for (const auto* bad :
         {"[\"jarr\"]", "[\"JAR\"]", "[\".jar\"]", "[\"zip\"]", "[\"\"]", "[1]", "\"jar\"", "true",
          "[\"jar\", \"nope\"]", "[[\"jar\"]]"}) {
      fixture.write("config.toml", std::string("[browser]\nopen_as_archive = ") + bad + "\n");
      TEST_CHECK(!service.reload());
      TEST_CHECK(
          !service.issues().empty()
          && service.issues().front().key == "browser.open_as_archive"
          && service.issues().front().message.contains("expected")
      );
      // The last valid settings stay.
      TEST_CHECK(namesOf(service.settings().browser.openAsArchive) == std::vector<std::string_view>{"jar"});
    }
  }

  void theSettingSavesAndInherits() {
    Fixture fixture;
    fixture.write("config.toml", "[browser]\nopen_as_archive = [\"jar\"]\n");
    luciole::ConfigService service(fixture.paths());
    TEST_CHECK(service.reload());
    const std::array chosen{luciole::OverrideEdit{luciole::BrowserPreference::OpenAsArchive, setOf({"epub", "docx"})}};
    TEST_CHECK(service.saveOverrides(chosen) == luciole::AtomicWriteState::Durable);
    TEST_CHECK(read(fixture.paths().overrides).contains("open_as_archive"));
    luciole::ConfigService reloaded(fixture.paths());
    TEST_CHECK(reloaded.reload());
    TEST_CHECK((namesOf(reloaded.settings().browser.openAsArchive) == std::vector<std::string_view>{"docx", "epub"}));
    // An empty choice is an override: the base file's list no longer applies.
    const std::array none{luciole::OverrideEdit{luciole::BrowserPreference::OpenAsArchive, ArchiveContainerSet{}}};
    TEST_CHECK(reloaded.saveOverrides(none) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService emptied(fixture.paths());
    TEST_CHECK(emptied.reload() && emptied.settings().browser.openAsArchive.none());
    TEST_CHECK(read(fixture.paths().overrides).contains("open_as_archive = []"));
    // Removing the override inherits again.
    const std::array inherit{luciole::OverrideEdit{luciole::BrowserPreference::OpenAsArchive, std::nullopt}};
    TEST_CHECK(emptied.saveOverrides(inherit) == luciole::AtomicWriteState::Durable);
    luciole::ConfigService inherited(fixture.paths());
    TEST_CHECK(inherited.reload());
    TEST_CHECK(namesOf(inherited.settings().browser.openAsArchive) == std::vector<std::string_view>{"jar"});
    TEST_CHECK(!read(fixture.paths().overrides).contains("open_as_archive"));
  }
} // namespace

int main() {
  theTableHoldsTheListedTypes();
  theDocsListEveryType();
  aGzipContainerNeedsTheGzipFilter();
  recognitionBySuffix();
  recognitionByMimeType();
  aRealArchiveNameBeatsTheMimeType();
  theExtractFolderDropsTheContainerSuffix();
  realArchivesAreNotContainers();
  theMenuAlwaysBrowsesAContainer();
  openFollowsTheSetting();
  thePreviewListsASwitchedOnContainer();
  theSettingDefaultsToNone();
  theSettingReadsTypeNames();
  theSettingRejectsWhatItDoesNotKnow();
  theSettingSavesAndInherits();
  std::println("archive-container: all checks passed");
  return 0;
}
