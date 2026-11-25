#include "archive_format_harness.h"
#include "fs/archive_container.h"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <sys/stat.h>
#include <vector>

// cpio archives: names, the formats libarchive writes, and the catalog rules tar entries already follow for links
// and special files.
namespace {
  using namespace luciole::test::formats;

  enum class Flavor { Newc, Odc, Bin };
  struct Node {
    std::string name, data;
    int type;
    unsigned nlink;
    long inode;
    std::string symlink;
    Node(std::string n, std::string d = {}, int t = AE_IFREG, unsigned links = 1, long ino = 0, std::string target = {})
        : name(std::move(n)), data(std::move(d)), type(t), nlink(links), inode(ino), symlink(std::move(target)) {}
  };

  void writeCpio(const fs::path& path, Flavor flavor, const std::vector<Node>& nodes) {
    const auto writer = newWriter();
    TEST_CHECK(
        (flavor == Flavor::Newc      ? archive_write_set_format_cpio_newc(writer.get())
             : flavor == Flavor::Odc ? archive_write_set_format_cpio_odc(writer.get())
                                     : archive_write_set_format_cpio_bin(writer.get()))
        == ARCHIVE_OK
    );
    TEST_CHECK(archive_write_add_filter_none(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& node : nodes) {
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), node.name.c_str());
      archive_entry_set_filetype(entry.get(), static_cast<unsigned>(node.type));
      archive_entry_set_perm(entry.get(), node.type == AE_IFDIR ? 0755 : 0644);
      archive_entry_set_dev(entry.get(), 1);
      archive_entry_set_ino64(
          entry.get(), node.inode != 0 ? node.inode : 100 + static_cast<long>(&node - nodes.data())
      );
      archive_entry_set_nlink(entry.get(), node.nlink);
      if (node.type == AE_IFLNK) {
        archive_entry_set_symlink(entry.get(), node.symlink.c_str());
      }
      if (node.type == AE_IFCHR) {
        archive_entry_set_rdevmajor(entry.get(), 1);
        archive_entry_set_rdevminor(entry.get(), 3);
      }
      archive_entry_set_size(entry.get(), static_cast<la_int64_t>(node.data.size()));
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      if (!node.data.empty()) {
        TEST_CHECK(
            archive_write_data(writer.get(), node.data.data(), node.data.size())
            == static_cast<la_ssize_t>(node.data.size())
        );
      }
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }

  const char* flavorName(Flavor flavor) {
    return flavor == Flavor::Newc ? "newc" : flavor == Flavor::Odc ? "odc" : "bin";
  }
  constexpr std::string_view kWords = "the quick brown fox\n";

  void namesAreRecognized() {
    TEST_CHECK(luciole::isBrowsableArchive(location("/tmp/initrd.cpio"), ""));
    TEST_CHECK(luciole::opensAsArchive(location("/tmp/INITRD.CPIO"), "", {}));
    TEST_CHECK(luciole::isBrowsableArchive(location("/tmp/opaque"), "application/x-cpio"));
    TEST_CHECK(luciole::archiveExtractFolderName(location("/tmp/initrd.cpio")) == "initrd");
    // Compressed cpio names are not part of this support.
    TEST_CHECK(!luciole::isBrowsableArchive(location("/tmp/initrd.cpio.gz"), ""));
    TEST_CHECK(!luciole::isBrowsableArchive(location("/tmp/initrd.cpio.xz"), ""));
  }

  void everyFlavorBrowsesAndUnpacks(const Folder& folder) {
    for (const auto flavor : {Flavor::Newc, Flavor::Odc, Flavor::Bin}) {
      const std::string label = flavorName(flavor);
      const auto path = folder.root / (label + ".cpio");
      writeCpio(
          path, flavor,
          {{"./dir", {}, AE_IFDIR},
           {"./dir/words.txt", std::string(kWords)},
           {"./empty", {}},
           {"./link", {}, AE_IFLNK, 1, 0, "dir/words.txt"}}
      );
      const auto found = inspect(path);
      TEST_CHECK(!found.error && found.catalog);
      TEST_CHECK(hasEntry(found, "dir") && hasEntry(found, "dir/words.txt") && hasEntry(found, "empty"));
      TEST_CHECK(hasEntry(found, "link"));
      const auto out = folder.root / ("out-" + label);
      TEST_CHECK(!extract(path, out).error);
      TEST_CHECK(read(out / "dir/words.txt") == kWords && fs::file_size(out / "empty") == 0);
      TEST_CHECK(fs::is_symlink(out / "link"));
    }
  }

  void unsafePathsAreRefused(const Folder& folder) {
    for (const auto flavor : {Flavor::Newc, Flavor::Odc}) {
      for (const auto* name : {"/etc/absolute", "../escaped", "dir/../../escaped"}) {
        const auto path = folder.root / "unsafe.cpio";
        writeCpio(path, flavor, {{"ok.txt", "fine"}, {name, "bad"}});
        const auto found = inspect(path);
        TEST_CHECK(found.error && !found.catalog);
        const auto out = folder.root / "unsafe-out";
        TEST_CHECK(extract(path, out).error && !fs::exists(out) && fs::is_empty(folder.root / "tmp"));
      }
    }
    TEST_CHECK(!fs::exists(folder.root / "escaped"));
  }

  // The same refusal a TAR with that entry gets, with the same message.
  void specialFilesFollowTheTarRule(const Folder& folder) {
    for (const int type : {AE_IFCHR, AE_IFIFO}) {
      const auto tar = folder.root / "special.tar";
      {
        const auto writer = newWriter();
        TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
        TEST_CHECK(archive_write_open_filename(writer.get(), tar.c_str()) == ARCHIVE_OK);
        addMember(writer.get(), {"node", {}, type});
        TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
      }
      const auto expected = inspect(tar);
      TEST_CHECK(expected.error && expected.error->kind == luciole::FileErrorKind::Unsupported);
      for (const auto flavor : {Flavor::Newc, Flavor::Odc}) {
        const auto path = folder.root / "special.cpio";
        writeCpio(path, flavor, {{"file", "x"}, {"node", {}, type}});
        const auto found = inspect(path);
        TEST_CHECK(found.error && !found.catalog && *found.error == *expected.error);
        const auto out = folder.root / "special-out";
        TEST_CHECK(extract(path, out).error && !fs::exists(out));
      }
    }
  }

  void odcHardLinksCarryTheirDataEverywhere(const Folder& folder) {
    const auto path = folder.root / "links-odc.cpio";
    writeCpio(path, Flavor::Odc, {{"a", "shared", AE_IFREG, 2, 7}, {"b", "shared", AE_IFREG, 2, 7}});
    const auto found = inspect(path);
    TEST_CHECK(!found.error && found.catalog);
    const auto out = folder.root / "links-odc-out";
    TEST_CHECK(!extract(path, out).error);
    TEST_CHECK(read(out / "a") == "shared" && read(out / "b") == "shared");
  }

  // One newc entry as GNU cpio and the kernel's initramfs format lay it out.
  std::string newcEntry(const std::string& name, const std::string& data, unsigned inode, unsigned nlink) {
    const auto hex = [](unsigned value) {
      char text[9];
      std::snprintf(text, sizeof text, "%08X", value);
      return std::string(text);
    };
    std::string bytes = "070701"
        + hex(inode)
        + hex(0100644)
        + hex(0)
        + hex(0)
        + hex(nlink)
        + hex(0)
        + hex(static_cast<unsigned>(data.size()))
        + hex(1)
        + hex(0)
        + hex(0)
        + hex(0)
        + hex(static_cast<unsigned>(name.size() + 1))
        + hex(0);
    bytes += name;
    bytes.append(1 + (4 - (bytes.size() + 1) % 4) % 4, '\0');
    bytes += data;
    bytes.append((4 - data.size() % 4) % 4, '\0');
    return bytes;
  }

  // newc stores a file's data with its last link only; an earlier link would extract as an empty file.
  void newcHardLinksWithDataAreRefused(const Folder& folder) {
    const auto path = folder.root / "links-newc.cpio";
    write(
        path,
        newcEntry("first", {}, 7, 3)
            + newcEntry("second", {}, 7, 3)
            + newcEntry("third", "shared", 7, 3)
            + newcEntry("TRAILER!!!", {}, 0, 1)
    );
    const auto found = inspect(path);
    TEST_CHECK(found.error && !found.catalog && found.error->kind == luciole::FileErrorKind::Unsupported);
    TEST_CHECK(found.error->message.contains("hard link"));
    const auto out = folder.root / "links-newc-out";
    const auto done = extract(path, out);
    TEST_CHECK(done.error && done.error->message.contains("hard link") && !fs::exists(out));
    // The same file written by the host's cpio.
    if (std::system("command -v cpio >/dev/null 2>&1") == 0) {
      const auto work = folder.root / "host-cpio";
      fs::create_directory(work);
      write(work / "one", "shared");
      fs::create_hard_link(work / "one", work / "two");
      const auto host = folder.root / "host-links.cpio";
      TEST_CHECK(
          std::system(("cd '"
                       + work.string()
                       + "' && printf 'one\\ntwo\\n' | cpio -o -H newc > '"
                       + host.string()
                       + "' 2>/dev/null")
                          .c_str())
          == 0
      );
      const auto refused = inspect(host);
      TEST_CHECK(refused.error && refused.error->message.contains("hard link"));
    } else {
      std::println(stderr, "skipped: no cpio program");
    }
    // Links of an empty file hold no data to lose.
    write(
        folder.root / "links-empty.cpio",
        newcEntry("x", {}, 9, 2) + newcEntry("y", {}, 9, 2) + newcEntry("TRAILER!!!", {}, 0, 1)
    );
    const auto fine = inspect(folder.root / "links-empty.cpio");
    TEST_CHECK(!fine.error && hasEntry(fine, "x") && hasEntry(fine, "y"));
    // libarchive's own writer keeps the data on the first link, which the catalog already resolves.
    const auto first = folder.root / "links-first.cpio";
    writeCpio(first, Flavor::Newc, {{"a", "shared", AE_IFREG, 2, 7}, {"b", "shared", AE_IFREG, 2, 7}});
    const auto firstOut = folder.root / "links-first-out";
    TEST_CHECK(!extract(first, firstOut).error && read(firstOut / "a") == "shared" && read(firstOut / "b") == "shared");
  }

  void brokenAndForeignFiles(const Folder& folder) {
    const auto path = folder.root / "cut.cpio";
    writeCpio(path, Flavor::Newc, {{"big", std::string(5000, 'x')}, {"after", "y"}});
    const auto bytes = read(path);
    write(path, bytes.substr(0, 2000));
    TEST_CHECK(inspect(path).error.has_value());
    write(folder.root / "text.cpio", "plain text, not an archive\n");
    TEST_CHECK(inspect(folder.root / "text.cpio").error.has_value());
    // A TAR whose first name looks like a cpio magic is still a TAR, as is a ZIP named like cpio.
    write(folder.root / "magic-name.cpio", plainTar({{"070701deadbeef", "t"}}));
    const auto asTar = inspect(folder.root / "magic-name.cpio");
    TEST_CHECK(!asTar.error && hasEntry(asTar, "070701deadbeef"));
  }
} // namespace

int main() {
  namesAreRecognized();
  const Folder folder;
  everyFlavorBrowsesAndUnpacks(folder);
  unsafePathsAreRefused(folder);
  specialFilesFollowTheTarRule(folder);
  odcHardLinksCarryTheirDataEverywhere(folder);
  newcHardLinksWithDataAreRefused(folder);
  brokenAndForeignFiles(folder);
  std::println("archive-cpio: all checks passed");
  return 0;
}
