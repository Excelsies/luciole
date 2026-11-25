#include "archive_ar_fixtures.h"
#include "archive_format_harness.h"
#include "fs/archive_container.h"

#include <cstdlib>
#include <print>
#include <string>
#include <vector>

// ar archives: names, the members of GNU and BSD libraries including their symbol tables and long-name tables, and
// the path rules every other format already follows.
namespace {
  using namespace luciole::test::formats;

  std::string objectBytes() {
    return {reinterpret_cast<const char*>(ar_fixtures::kObject), sizeof ar_fixtures::kObject};
  }
  bool haveTool(const char* name) {
    return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
  }

  // One ar member header and body, as every ar flavor lays them out.
  std::string arMember(std::string name, const std::string& data) {
    const auto field = [](std::string text, std::size_t width) {
      TEST_CHECK(text.size() <= width);
      return text.append(width - text.size(), ' ');
    };
    auto bytes = field(std::move(name), 16)
        + field("0", 12)
        + field("0", 6)
        + field("0", 6)
        + field("100644", 8)
        + field(std::to_string(data.size()), 10)
        + "`\n"
        + data;
    if (data.size() % 2 != 0) {
      bytes += '\n';
    }
    return bytes;
  }
  const std::string kMagic = "!<arch>\n";

  std::vector<std::string> paths(const Outcome& outcome) {
    std::vector<std::string> found;
    for (const auto& entry : outcome.catalog->entries) {
      found.push_back(entry.path);
    }
    std::ranges::sort(found);
    return found;
  }

  void namesAreRecognized() {
    for (const auto* name : {"/tmp/libfoo.a", "/tmp/libfoo.ar", "/tmp/LIBFOO.A"}) {
      TEST_CHECK(luciole::isBrowsableArchive(location(name), ""));
      TEST_CHECK(luciole::opensAsArchive(location(name), "", {}));
    }
    TEST_CHECK(luciole::archiveExtractFolderName(location("/tmp/libfoo.a")) == "libfoo");
    TEST_CHECK(luciole::archiveExtractFolderName(location("/tmp/libfoo.AR")) == "libfoo");
    TEST_CHECK(luciole::isBrowsableArchive(location("/tmp/opaque"), "application/x-archive"));
    TEST_CHECK(!luciole::isBrowsableArchive(location("/tmp/.a"), ""));
    TEST_CHECK(!luciole::isBrowsableArchive(location("/tmp/data.arx"), ""));
    TEST_CHECK(!luciole::isBrowsableArchive(location("/tmp/pkg.deb"), ""));
  }

  void gnuLibraryWithEveryTable(const Folder& folder) {
    // The symbol table "/" and the long-name table "//" are not members; "/0" names its member through the table.
    const auto path = folder.root / "gnu.a";
    write(
        path,
        kMagic
            + arMember("/", std::string("\0\0\0\1\0\0\0\x10sym\0", 12))
            + arMember("//", "long_member_name_x.o/\n")
            + arMember("/0", "abc")
            + arMember("short.o/", objectBytes())
    );
    const auto found = inspect(path);
    TEST_CHECK(!found.error && found.catalog);
    TEST_CHECK((paths(found) == std::vector<std::string>{"long_member_name_x.o", "short.o"}));
    const auto out = folder.root / "gnu-out";
    TEST_CHECK(!extract(path, out).error);
    TEST_CHECK(read(out / "long_member_name_x.o") == "abc" && read(out / "short.o") == objectBytes());
    TEST_CHECK(!fs::exists(out / "SYM64"));
    // An odd-sized last member is followed by one padding byte, which is not a truncated header.
    write(folder.root / "odd.a", kMagic + arMember("odd.o/", "abc"));
    const auto odd = inspect(folder.root / "odd.a");
    TEST_CHECK(!odd.error && (paths(odd) == std::vector<std::string>{"odd.o"}));
    write(folder.root / "empty.a", kMagic);
    const auto empty = inspect(folder.root / "empty.a");
    TEST_CHECK(!empty.error && empty.catalog && empty.catalog->entries.empty());
  }

  void libraryWrittenByTheHostAr(const Folder& folder) {
    if (!haveTool("ar")) {
      std::println(stderr, "skipped: no ar program");
      return;
    }
    const auto work = folder.root / "host-ar";
    fs::create_directory(work);
    write(work / "foo.o", objectBytes());
    write(work / "this_is_a_really_long_member_name.o", objectBytes() + "tail");
    const auto library = work / "libhost.a";
    TEST_CHECK(
        std::system(
            ("cd '" + work.string() + "' && ar rcs libhost.a foo.o this_is_a_really_long_member_name.o").c_str()
        )
        == 0
    );
    // As written: the symbol table member leads the file.
    TEST_CHECK(read(library).starts_with(kMagic + "/ "));
    const auto found = inspect(library);
    TEST_CHECK(!found.error && found.catalog);
    TEST_CHECK((paths(found) == std::vector<std::string>{"foo.o", "this_is_a_really_long_member_name.o"}));
    const auto out = folder.root / "host-ar-out";
    TEST_CHECK(!extract(library, out).error);
    TEST_CHECK(read(out / "foo.o") == objectBytes());
    TEST_CHECK(read(out / "this_is_a_really_long_member_name.o") == objectBytes() + "tail");
  }

  void bsdLibrary(const Folder& folder) {
    const auto path = folder.root / "bsd.a";
    {
      const auto writer = newWriter();
      TEST_CHECK(archive_write_set_format_ar_bsd(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
      addMember(writer.get(), {"a_member_with_a_long_name.o", "long"});
      addMember(writer.get(), {"b.o", "short"});
      TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    }
    TEST_CHECK(read(path).contains("#1/"));
    const auto found = inspect(path);
    TEST_CHECK(!found.error && (paths(found) == std::vector<std::string>{"a_member_with_a_long_name.o", "b.o"}));
    // The symbol tables of BSD and 64-bit GNU libraries are indexes, not members.
    for (const auto* index : {"__.SYMDEF", "__.SYMDEF SORTED", "/SYM64/"}) {
      const auto library = folder.root / "indexed.a";
      write(library, kMagic + arMember(index, std::string(24, '\0')) + arMember("real.o/", "data"));
      const auto indexed = inspect(library);
      TEST_CHECK(!indexed.error && (paths(indexed) == std::vector<std::string>{"real.o"}));
      const auto out = folder.root / "indexed-out";
      TEST_CHECK(!extract(library, out).error && read(out / "real.o") == "data" && fs::exists(out / "real.o"));
      fs::remove_all(out);
    }
  }

  void unsafeAndBrokenLibrariesFail(const Folder& folder) {
    const std::vector<std::pair<std::string, std::string>> bad{
        {"parent", kMagic + arMember("//", "../escape.o/\n") + arMember("/0", "x")},
        {"absolute", kMagic + arMember("//", "/abs.o/\n") + arMember("/0", "x")},
        {"bsd-parent", kMagic + arMember("#1/12", std::string("../escape.o\0data", 16))},
        {"duplicate", kMagic + arMember("a.o/", "1") + arMember("a.o/", "2")},
        {"dangling-long-name", kMagic + arMember("a.o/", "1") + arMember("/99", "2")},
        {"truncated-body", (kMagic + arMember("a.o/", std::string(100, 'x'))).substr(0, 8 + 60 + 40)},
        {"not-an-archive", "!<arch>\nthis is not a header at all, only text of some length"},
    };
    for (const auto& [name, bytes] : bad) {
      const auto path = folder.root / ("bad-" + name + ".a");
      write(path, bytes);
      const auto found = inspect(path);
      TEST_CHECK(found.error && !found.catalog);
      const auto out = folder.root / ("bad-out-" + name);
      TEST_CHECK(extract(path, out).error && !fs::exists(out) && fs::is_empty(folder.root / "tmp"));
    }
    TEST_CHECK(!fs::exists(folder.root / "escape.o"));
    // A member larger than the limit is refused like any other format's.
    write(folder.root / "limit.a", kMagic + arMember("big.o/", std::string(4096, 'x')));
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 1024;
    const auto found = inspect(folder.root / "limit.a", limits);
    TEST_CHECK(found.error && found.error->kind == luciole::FileErrorKind::Unsupported);
  }

  void otherFormatsStillWin(const Folder& folder) {
    write(folder.root / "plain.a", "plain text, not an archive\n");
    TEST_CHECK(inspect(folder.root / "plain.a").error.has_value());
    // A TAR named like a library is still read as a TAR.
    write(folder.root / "tar-inside.a", plainTar({{"member.txt", "t"}}));
    const auto asTar = inspect(folder.root / "tar-inside.a");
    TEST_CHECK(!asTar.error && hasEntry(asTar, "member.txt"));
  }
} // namespace

int main() {
  namesAreRecognized();
  const Folder folder;
  gnuLibraryWithEveryTable(folder);
  libraryWrittenByTheHostAr(folder);
  bsdLibrary(folder);
  unsafeAndBrokenLibrariesFail(folder);
  otherFormatsStillWin(folder);
  std::println("archive-ar: all checks passed");
  return 0;
}
