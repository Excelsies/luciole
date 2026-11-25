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
    for (const auto* uri : {"sftp://host/libfoo.a", "sftp://host/libfoo.ar"}) {
      const auto remote = luciole::Location::fromUri(uri);
      TEST_CHECK(!remote.isNative());
      TEST_CHECK(!luciole::isBrowsableArchive(remote, "application/x-archive"));
      TEST_CHECK(!luciole::opensAsArchive(remote, "application/x-archive", {}));
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

  // A member header declaring `size` bytes of which none follow: libarchive allocates by the declared size.
  std::string arHeaderOnly(std::string name, std::uint64_t size) {
    auto head = arMember(std::move(name), "");
    head.replace(48, 10, std::to_string(size).append(10 - std::to_string(size).size(), ' '));
    return head.substr(0, 60);
  }
  // A plain ar file whose member declares `size` bytes, as a sparse file so the test itself holds no memory.
  void writeSparseAr(const fs::path& path, const std::string& name, std::uint64_t size) {
    write(path, kMagic + arHeaderOnly(name, size));
    fs::resize_file(path, 8 + 60 + size);
  }
  constexpr std::size_t kCapHeadroom = 64U * 1024U * 1024U;
  constexpr std::uint64_t kHugeTable = 96ULL * 1024 * 1024;

  void oversizedNameTablesAreRefusedWithoutAllocating(const Folder& folder) {
    // A plain file: libarchive holds the declared size of the "//" table in memory, twice over.
    {
      const auto path = folder.root / "huge-table.a";
      writeSparseAr(path, "//", kHugeTable);
      const AddressCap cap(kCapHeadroom);
      const auto found = inspect(path);
      TEST_CHECK(!found.catalog && found.error && found.error->kind == luciole::FileErrorKind::Unsupported);
      TEST_CHECK(found.error->message.contains("long-name table"));
      const auto out = folder.root / "huge-out";
      const auto done = extract(path, out);
      TEST_CHECK(done.error && done.error->message.contains("long-name table") && !fs::exists(out));
    }
    // A table behind other members is found by walking the headers.
    write(
        folder.root / "late-table.a",
        kMagic + arMember("a.o/", "1") + arMember("b.o/", "22") + arHeaderOnly("//", kHugeTable)
    );
    fs::resize_file(folder.root / "late-table.a", fs::file_size(folder.root / "late-table.a") + kHugeTable);
    {
      const AddressCap cap(kCapHeadroom);
      const auto late = inspect(folder.root / "late-table.a");
      TEST_CHECK(late.error && late.error->message.contains("long-name table"));
    }
    // A table at the limit still opens.
    luciole::ArchiveLimits limits;
    limits.maxMetadataBytes = 4096;
    write(folder.root / "fits.a", kMagic + arMember("//", std::string(4094, 'n') + "/\n") + arMember("a.o/", "1"));
    const auto fits = inspect(folder.root / "fits.a", limits);
    TEST_CHECK(!fits.error && hasEntry(fits, "a.o"));
    write(folder.root / "over.a", kMagic + arMember("//", std::string(4096, 'n') + "/\n") + arMember("a.o/", "1"));
    TEST_CHECK(inspect(folder.root / "over.a", limits).error.has_value());
    TEST_CHECK(tmpIsEmpty(folder));
  }

  // A header with its fields exactly as given, each padded with spaces to its width.
  std::string rawHeader(const std::string& name, const std::string& size, const std::string& end = "`\n") {
    const auto field = [](std::string text, std::size_t width) {
      TEST_CHECK(text.size() <= width);
      return text.append(width - text.size(), ' ');
    };
    return field(name, 16)
        + field("0", 12)
        + field("0", 6)
        + field("0", 6)
        + field("100644", 8)
        + field(size, 10)
        + end;
  }
  // `head` (headers and small members) followed by a sparse hole that makes the file as long as its last header says.
  void writeSparseAfter(const fs::path& path, const std::string& head, std::uint64_t hole) {
    write(path, kMagic + head);
    fs::resize_file(path, fs::file_size(path) + hole);
  }
  std::string refusalOf(const fs::path& path) {
    const AddressCap cap(kCapHeadroom);
    const auto found = inspect(path);
    TEST_CHECK(!found.catalog && found.error);
    return found.error ? found.error->message : std::string();
  }

  // libarchive accepts size fields and names that are not what ar writes; the walk must not be more lenient than it.
  void disguisedTablesAreRefusedWithoutAllocating(const Folder& folder) {
    const auto big = std::to_string(kHugeTable);
    const std::string leadingMember = "x\n";
    struct Variant {
      std::string label, head, expected;
    };
    const std::string table = "long-name table", malformed = "malformed member header";
    const std::vector<Variant> variants{
        {"first-lead-space", rawHeader("a.o/", " 1") + leadingMember + rawHeader("//", big), malformed},
        {"first-five-spaces", rawHeader("a.o/", "     1") + leadingMember + rawHeader("//", big), malformed},
        {"first-nul", rawHeader("a.o/", std::string("1\0", 2)) + leadingMember + rawHeader("//", big), malformed},
        {"first-letter", rawHeader("a.o/", "1x") + leadingMember + rawHeader("//", big), malformed},
        {"name-nul", rawHeader(std::string("//\0", 3), big), table},
        {"name-three-slashes", rawHeader("///", big), table},
        {"name-letter", rawHeader("//x", big), table},
        {"size-lead-space", rawHeader("//", " " + big), malformed},
        {"size-nul", rawHeader("//", big + std::string(1, '\0')), malformed},
        {"size-letter", rawHeader("//", big + "x"), malformed},
        {"size-plus", rawHeader("//", "+" + big), malformed},
        {"size-leading-zero", rawHeader("//", "0" + big), table},
        {"bad-end", rawHeader("a.o/", "1", "XX") + leadingMember, malformed},
    };
    for (const auto& variant : variants) {
      const auto path = folder.root / ("disguised-" + variant.label + ".a");
      writeSparseAfter(path, variant.head, variant.head.contains("//") ? kHugeTable : 0);
      TEST_CHECK(refusalOf(path).contains(variant.expected));
    }
    TEST_CHECK(tmpIsEmpty(folder));
  }

  // Random damage to the name and size fields of a table header: every file is refused or opens within bounds.
  void damagedTableHeadersNeverAllocateTheTable(const Folder& folder) {
    constexpr std::string_view alphabet = " \0/x\t+09";
    std::uint32_t state = 20240607;
    const auto pick = [&](std::size_t bound) {
      state = state * 1664525U + 1013904223U;
      return static_cast<std::size_t>(state >> 8) % bound;
    };
    for (int round = 0; round < 60; ++round) {
      std::string name(16, ' '), size = std::to_string(kHugeTable);
      name.replace(0, 2, "//");
      size.resize(10, ' ');
      for (int hits = 0; hits < 3; ++hits) {
        auto& field = pick(2) == 0 ? name : size;
        field[pick(field.size() < 6 ? field.size() : 6)] = alphabet[pick(alphabet.size())];
      }
      const auto path = folder.root / "damaged.a";
      writeSparseAfter(path, rawHeader(name, size), kHugeTable);
      const AddressCap cap(kCapHeadroom);
      const auto found = inspect(path);
      // An allocation that fails under the cap would surface as an error that is neither of ours nor libarchive's.
      TEST_CHECK(found.error || found.catalog);
      if (found.error) {
        const auto& message = found.error->message;
        TEST_CHECK(!message.contains("memory") && !message.contains("allocate") && !message.contains("Can't"));
      }
    }
  }

  void compressedArchivesNeverReachTheArReader(const Folder& folder) {
    // The decoded bytes of a gzip start like an ar archive, one with a huge "//" table and one that is fine.
    const std::string normal = kMagic + arMember("//", "long_member_name_x.o/\n") + arMember("/0", "abc");
    for (const auto* label : {"table", "normal"}) {
      const auto path = folder.root / (std::string("wrapped-") + label + ".tar.gz");
      {
        const auto body = std::string(label) == "normal"
            ? normal
            : kMagic + arHeaderOnly("//", kHugeTable) + std::string(kHugeTable, '\0');
        compressStream(path, ARCHIVE_FILTER_GZIP, body);
      }
      const AddressCap cap(kCapHeadroom);
      const auto found = inspect(path);
      TEST_CHECK(found.error && !found.catalog);
      const auto out = folder.root / (std::string("wrapped-out-") + label);
      TEST_CHECK(extract(path, out).error && !fs::exists(out));
    }
    TEST_CHECK(tmpIsEmpty(folder));
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
  oversizedNameTablesAreRefusedWithoutAllocating(folder);
  disguisedTablesAreRefusedWithoutAllocating(folder);
  damagedTableHeadersNeverAllocateTheTable(folder);
  compressedArchivesNeverReachTheArReader(folder);
  otherFormatsStillWin(folder);
  std::println("archive-ar: all checks passed");
  return 0;
}
