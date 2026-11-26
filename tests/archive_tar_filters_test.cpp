#include "archive_format_harness.h"
#include "fs/archive_container.h"
#include "fs/archive_type.h"
#include "operations/archive_lzma_header.h"

#include <archive.h>
#include <cctype>
#include <lzma.h>
#include <print>
#include <string>
#include <vector>

// The compressed TARs beyond gzip, bzip2, zstd and xz: lz4, lzma, lzip and compress. Recognition by name and content
// type, browsing and unpacking through the archive service, and the limits that hold for every one of them.
namespace {
  using namespace luciole::test::formats;
  using luciole::ArchiveLimits;

  struct Kind {
    std::string_view label;
    std::vector<std::string_view> suffixes;
    std::string_view mime, plainSuffix, plainMime;
    int filter;
    bool available;
  };
  const std::vector<Kind>& kinds() {
    static const std::vector<Kind> all{
        {"lz4",
         {".tar.lz4", ".tlz4"},
         "application/x-lz4-compressed-tar",
         ".lz4",
         "application/x-lz4",
         ARCHIVE_FILTER_LZ4,
         archive_liblz4_version() != nullptr},
        {"lzma",
         {".tar.lzma", ".tlz"},
         "application/x-lzma-compressed-tar",
         ".lzma",
         "application/x-lzma",
         ARCHIVE_FILTER_LZMA,
         lzma_filter_decoder_is_supported(LZMA_FILTER_LZMA1) != 0},
        {"lzip",
         {".tar.lz"},
         "application/x-lzip-compressed-tar",
         ".lz",
         "application/x-lzip",
         ARCHIVE_FILTER_LZIP,
         lzma_filter_decoder_is_supported(LZMA_FILTER_LZMA1) != 0},
        {"compress",
         {".tar.Z", ".taz"},
         "application/x-tarz",
         ".Z",
         "application/x-compress",
         ARCHIVE_FILTER_COMPRESS,
         true},
    };
    return all;
  }

  void namesAreRecognized() {
    for (const auto& kind : kinds()) {
      for (const auto suffix : kind.suffixes) {
        const auto named = location("/tmp/logs" + std::string(suffix));
        TEST_CHECK(luciole::isBrowsableArchive(named, "") == kind.available);
        TEST_CHECK(luciole::opensAsArchive(named, "", {}) == kind.available);
        if (!kind.available) {
          continue;
        }
        TEST_CHECK(luciole::archiveExtractFolderName(named) == "logs");
        std::string upper(suffix);
        std::ranges::transform(upper, upper.begin(), [](char c) { return static_cast<char>(std::toupper(c)); });
        const auto shouting = location("/tmp/LOGS" + upper);
        TEST_CHECK(luciole::isBrowsableArchive(shouting, ""));
        TEST_CHECK(luciole::archiveExtractFolderName(shouting) == "LOGS");
      }
      // The content type alone decides for a file whose name says nothing.
      TEST_CHECK(luciole::isBrowsableArchive(location("/tmp/opaque"), std::string(kind.mime)) == kind.available);
      // A remote location is never an archive, whatever its name or content type.
      for (const auto suffix : kind.suffixes) {
        const auto remote = luciole::Location::fromUri("sftp://host/logs" + std::string(suffix));
        TEST_CHECK(!remote.isNative());
        TEST_CHECK(!luciole::isBrowsableArchive(remote, std::string(kind.mime)));
        TEST_CHECK(!luciole::opensAsArchive(remote, std::string(kind.mime), {}));
      }
      // A single compressed file is not an archive, whatever its content type.
      const auto plain = location("/tmp/notes" + std::string(kind.plainSuffix));
      TEST_CHECK(!luciole::isBrowsableArchive(plain, ""));
      TEST_CHECK(!luciole::isBrowsableArchive(plain, std::string(kind.plainMime)));
      TEST_CHECK(!luciole::opensAsArchive(plain, std::string(kind.plainMime), {}));
    }
  }

  std::string pseudoRandom(std::size_t size) {
    std::string bytes(size, '\0');
    std::uint32_t state = 12345;
    for (auto& byte : bytes) {
      state = state * 1664525U + 1013904223U;
      byte = static_cast<char>(state >> 24);
    }
    return bytes;
  }
  const std::vector<Member>& sampleMembers() {
    static const std::vector<Member> members{
        {"docs", {}, AE_IFDIR},
        {"docs/a.txt", "alpha\n"},
        {"big.bin", pseudoRandom(200000)},
        {"to-a", {}, AE_IFLNK, "docs/a.txt"},
    };
    return members;
  }
  fs::path makeFile(const Folder& folder, const Kind& kind, std::string_view name, std::string_view tarBytes) {
    const auto path = folder.root / (std::string(name) + std::string(kind.suffixes.front()));
    compressStream(path, kind.filter, tarBytes);
    return path;
  }

  void browsesAndUnpacks(const Folder& folder, const Kind& kind) {
    for (const auto suffix : kind.suffixes) {
      const auto path = folder.root / (std::string("round-") + std::string(kind.label) + std::string(suffix));
      compressStream(path, kind.filter, plainTar(sampleMembers()));
      const auto outcome = inspect(path);
      TEST_CHECK(!outcome.error && outcome.catalog);
      TEST_CHECK(hasEntry(outcome, "docs") && hasEntry(outcome, "docs/a.txt") && hasEntry(outcome, "big.bin"));
      TEST_CHECK(hasEntry(outcome, "to-a"));
      // The archive page names the compression it read.
      TEST_CHECK(!outcome.catalog->compression.empty());
      if (kind.filter == ARCHIVE_FILTER_LZMA || kind.filter == ARCHIVE_FILTER_LZIP) {
        TEST_CHECK(outcome.catalog->compression == kind.label);
      }
      const auto out = folder.root / ("out-" + std::string(kind.label) + std::string(suffix));
      const auto done = extract(path, out);
      TEST_CHECK(!done.error);
      TEST_CHECK(read(out / "docs/a.txt") == "alpha\n");
      TEST_CHECK(read(out / "big.bin") == pseudoRandom(200000));
      TEST_CHECK(fs::is_symlink(out / "to-a"));
      // One selected member.
      const auto one = folder.root / ("one-" + std::string(kind.label) + std::string(suffix));
      TEST_CHECK(!extract(path, one, {"docs/a.txt"}).error && read(one / "docs/a.txt") == "alpha\n");
      TEST_CHECK(!fs::exists(one / "big.bin"));
    }
    TEST_CHECK(tmpIsEmpty(folder));
  }

  // The TAR ends well before the stream does, so only a pass over the whole stream sees its framing.
  std::string paddedTar() { return plainTar({{"file", "abc"}}) + std::string(4U * 1024U * 1024U, '\0'); }

  void rejected(const Folder& folder, const std::string& source, const std::string& why, ArchiveLimits limits = {}) {
    const auto path = folder.root / ("bad-" + why);
    write(path, source);
    const auto found = inspect(path, limits);
    TEST_CHECK(found.error && !found.catalog);
    const auto out = folder.root / ("bad-out-" + why);
    const auto done = extract(path, out, {}, limits);
    TEST_CHECK(done.error && !fs::exists(out) && tmpIsEmpty(folder));
  }

  void brokenStreamsFail(const Folder& folder, const Kind& kind) {
    const std::string label(kind.label);
    const auto suffix = std::string(kind.suffixes.front());
    // Cut inside a member's data, which every stream notices.
    const auto sample = read(makeFile(folder, kind, "cut-" + label, plainTar(sampleMembers())));
    rejected(folder, sample.substr(0, sample.size() / 2), label + "-half" + suffix);
    const auto good = makeFile(folder, kind, "padded-" + label, paddedTar());
    const auto bytes = read(good);
    TEST_CHECK(!inspect(good).error);
    // The LZW stream of compress carries no trailer: losing only padding cannot be seen, so only the others check.
    if (kind.filter != ARCHIVE_FILTER_COMPRESS) {
      rejected(folder, bytes.substr(0, bytes.size() - 1), label + "-minus-one" + suffix);
      auto flipped = bytes;
      flipped[flipped.size() - 3] = static_cast<char>(flipped[flipped.size() - 3] ^ 0x01);
      rejected(folder, flipped, label + "-flipped" + suffix);
    }
  }

  void decodedBombHitsTheLimit(const Folder& folder, const Kind& kind) {
    const auto path = makeFile(folder, kind, std::string("bomb-") + std::string(kind.label), paddedTar());
    ArchiveLimits limits;
    limits.maxExpandedBytes = 128;
    limits.maxMetadataBytes = 256;
    limits.maxEntries = 4;
    const auto found = inspect(path, limits);
    TEST_CHECK(found.error && found.error->kind == luciole::FileErrorKind::Unsupported);
    TEST_CHECK(found.error->message.contains("decoded-container"));
    const auto out = folder.root / ("bomb-out-" + std::string(kind.label));
    const auto done = extract(path, out, {}, limits);
    TEST_CHECK(done.error && done.error->message.contains("decoded-container") && !fs::exists(out));
    TEST_CHECK(tmpIsEmpty(folder));
  }

  // The header field that sets the decoder's dictionary: bytes 1 to 4 of LZMA-alone, byte 5 of lzip.
  std::string withDictionary(std::string bytes, const Kind& kind, unsigned log2) {
    if (kind.filter == ARCHIVE_FILTER_LZMA) {
      TEST_CHECK(bytes.size() > 14);
      for (unsigned i = 0; i < 4; ++i) {
        bytes[1 + i] = static_cast<char>(((std::uint64_t{1} << log2) >> (8 * i)) & 0xFF);
      }
    } else {
      TEST_CHECK(bytes.starts_with("LZIP") && bytes.size() > 6);
      bytes[5] = static_cast<char>(log2);
    }
    return bytes;
  }

  void oversizedDictionaryIsRefused(const Folder& folder, const Kind& kind) {
    const std::string label(kind.label);
    const auto source = read(makeFile(folder, kind, "dict-" + label, plainTar(sampleMembers())));
    // A 64 MiB dictionary is within the limit and the same data still decodes.
    write(folder.root / ("dict-ok-" + label), withDictionary(source, kind, 26));
    const auto fits = inspect(folder.root / ("dict-ok-" + label));
    TEST_CHECK(!fits.error && hasEntry(fits, "docs/a.txt"));
    // 256 MiB and 1 GiB (lzip stops at 512 MiB) are not, and nothing large is allocated to find out.
    for (const unsigned log2 : {28U, 29U}) {
      const auto path = folder.root / ("dict-big-" + label + std::to_string(log2));
      write(path, withDictionary(source, kind, log2));
      const AddressCap cap(200U * 1024U * 1024U);
      const auto found = inspect(path);
      TEST_CHECK(!found.catalog && found.error && found.error->kind == luciole::FileErrorKind::Unsupported);
      TEST_CHECK(found.error->message.contains("128 MiB"));
      const auto out = folder.root / ("dict-out-" + label + std::to_string(log2));
      const auto done = extract(path, out);
      TEST_CHECK(done.error && done.error->message.contains("128 MiB") && !fs::exists(out));
    }
    TEST_CHECK(tmpIsEmpty(folder));
  }

  using luciole::archive_work::LzmaContainer;
  using luciole::archive_work::lzmaContainerOf;
  std::optional<LzmaContainer> classify(std::vector<unsigned char> head) { return lzmaContainerOf(head); }
  std::vector<unsigned char> padded(std::vector<unsigned char> head, std::size_t size = 14) {
    head.resize(size, 0);
    return head;
  }
  void aloneIsRecognizedOnlyByAWrittenHeader() {
    const std::vector<unsigned char> written{0x5D, 0, 0, 0x80, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    TEST_CHECK(classify(written) == LzmaContainer::Alone);
    auto known = written;
    std::fill(known.begin() + 5, known.begin() + 13, 0);
    known[5] = 0x10; // a known size of 16 bytes
    TEST_CHECK(classify(known) == LzmaContainer::Alone);
    auto threeHalves = written; // 2^23 + 2^22
    threeHalves[3] = 0xC0;
    TEST_CHECK(classify(threeHalves) == LzmaContainer::Alone);
    for (const auto& head : {
             std::vector<unsigned char>(written.begin(), written.begin() + 13), // too short
             [&] {
               auto h = written;
               h[0] = 225;
               return h;
             }(), // properties out of range
             [&] {
               auto h = written;
               h[3] = 0x81;
               return h;
             }(), // dictionary not 2^n or 2^n+2^(n-1)
             [&] {
               auto h = written;
               h[2] = 0;
               h[3] = 0;
               return h;
             }(), // dictionary below 4 KiB
             [&] {
               auto h = written;
               h[12] = 0x40;
               return h;
             }(), // size of 2^62 bytes
             [&] {
               auto h = written;
               h[13] = 1;
               return h;
             }(), // range coder must start with 0
         }) {
      TEST_CHECK(!classify(head));
    }
    TEST_CHECK(classify(padded({0xFD, '7', 'z', 'X', 'Z', 0})) == LzmaContainer::Xz);
    TEST_CHECK(classify({'L', 'Z', 'I', 'P', 1, 0x17}) == LzmaContainer::Lzip);
    TEST_CHECK(classify({'L', 'Z', 'I', 'P', 0, 0x17}) == LzmaContainer::Lzip);
    TEST_CHECK(!classify({'L', 'Z', 'I', 'P', 2, 0x17}));
    // The starts of the formats this app reads, and of files that are none of them.
    const std::vector<std::vector<unsigned char>> others{
        padded({'P', 'K', 3, 4, 20, 0}),
        padded({'7', 'z', 0xBC, 0xAF, 0x27, 0x1C, 0, 4}),
        padded({'R', 'a', 'r', '!', 0x1A, 7, 1, 0}),
        padded({0x1F, 0x8B, 8, 0}),
        padded({'B', 'Z', 'h', '9'}),
        padded({0x28, 0xB5, 0x2F, 0xFD}),
        padded({0x04, 0x22, 0x4D, 0x18}),
        padded({0x1F, 0x9D, 0x90}),
        padded({'!', '<', 'a', 'r', 'c', 'h', '>', '\n'}),
        padded({'0', '7', '0', '7', '0', '1'}),
        padded({'a'}), // a TAR whose first name is one letter
        padded({'a', 'b', 'c'}),
        padded({0x7F, 'E', 'L', 'F', 2, 1, 1}),
        padded({'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '\n'}),
        padded({}),
    };
    for (const auto& head : others) {
      TEST_CHECK(!classify(head));
    }
    // And no run of pseudo-random bytes is taken for LZMA.
    const auto noise = pseudoRandom(14 * 20000);
    unsigned taken = 0;
    for (std::size_t at = 0; at + 14 <= noise.size(); at += 14) {
      std::vector<unsigned char> head(
          noise.begin() + static_cast<long>(at), noise.begin() + static_cast<long>(at) + 14
      );
      taken += classify(head).has_value() ? 1U : 0U;
    }
    TEST_CHECK(taken == 0);
  }

  void filesOfOtherFormatsKeepTheirReader(const Folder& folder) {
    // A ZIP and a plain TAR under LZMA names still open as what they are.
    const auto zip = folder.root / "really-zip.tar.lzma";
    {
      const auto writer = newWriter();
      TEST_CHECK(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_open_filename(writer.get(), zip.c_str()) == ARCHIVE_OK);
      addMember(writer.get(), {"inside.txt", "zip"});
      TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    }
    const auto asZip = inspect(zip);
    TEST_CHECK(!asZip.error && hasEntry(asZip, "inside.txt"));
    write(folder.root / "really-tar.tlz", plainTar({{"a", "x"}}));
    const auto asTar = inspect(folder.root / "really-tar.tlz");
    TEST_CHECK(!asTar.error && hasEntry(asTar, "a"));
    // A file that only starts like LZMA-alone is not decoded as LZMA.
    const std::string lookalike("\x5D\x00\x00\x80\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x07garbage garbage", 29);
    for (const auto& body : {lookalike, std::string("plain text, not an archive\n"), pseudoRandom(5000)}) {
      write(folder.root / "fake.tar.lzma", body);
      const auto fake = inspect(folder.root / "fake.tar.lzma");
      TEST_CHECK(fake.error && !fake.catalog && !fake.error->message.contains("LZMA"));
    }
  }

  void lzipMembersAndTrailingData(const Folder& folder) {
    const auto& kind = kinds()[2];
    TEST_CHECK(kind.filter == ARCHIVE_FILTER_LZIP);
    const auto tar = plainTar(sampleMembers());
    const auto single = read(makeFile(folder, kind, "member1", tar));
    TEST_CHECK(!inspect(folder.root / "member1.tar.lz").error);
    // One TAR in two members, as tools that compress in blocks write it, opens and unpacks whole.
    const auto cut = std::size_t{4096};
    const auto first = read(makeFile(folder, kind, "half-a", tar.substr(0, cut)));
    const auto second = read(makeFile(folder, kind, "half-b", tar.substr(cut)));
    write(folder.root / "two-members.tar.lz", first + second);
    const auto both = inspect(folder.root / "two-members.tar.lz");
    TEST_CHECK(!both.error && hasEntry(both, "docs/a.txt") && hasEntry(both, "big.bin"));
    const auto out = folder.root / "two-members-out";
    TEST_CHECK(!extract(folder.root / "two-members.tar.lz", out).error);
    TEST_CHECK(read(out / "big.bin") == pseudoRandom(200000) && read(out / "docs/a.txt") == "alpha\n");
    // Whatever follows the last member is refused, down to a cut piece of the next member's magic number.
    const auto refusedWith = [&](const std::string& bytes, const std::string& name) {
      rejected(folder, bytes, name);
      const auto found = inspect(folder.root / ("bad-" + name));
      TEST_CHECK(found.error && found.error->message.contains("Lzip archive has data after its end"));
    };
    refusedWith(single + "junk", "lz-junk.tar.lz");
    refusedWith(single + std::string(512, '\0'), "lz-zeros.tar.lz");
    refusedWith(first + second + "trailing bytes", "lz-two-junk.tar.lz");
    for (const auto* piece : {"L", "LZ", "LZI"}) {
      const auto name = std::string("lz-magic-") + piece + ".tar.lz";
      rejected(folder, single + piece, name);
      const auto found = inspect(folder.root / ("bad-" + name));
      TEST_CHECK(found.error && found.error->message.contains("Lzip archive is corrupt or truncated"));
    }
  }

  void trailingDataAfterLzmaAloneFails(const Folder& folder) {
    const auto& kind = kinds()[1];
    const auto good = read(makeFile(folder, kind, "tail", plainTar(sampleMembers())));
    TEST_CHECK(!inspect(folder.root / ("tail" + std::string(kind.suffixes.front()))).error);
    rejected(folder, good + "junk", "alone-trailing.tar.lzma");
  }
} // namespace

int main() {
  namesAreRecognized();
  aloneIsRecognizedOnlyByAWrittenHeader();
  const Folder folder;
  filesOfOtherFormatsKeepTheirReader(folder);
  trailingDataAfterLzmaAloneFails(folder);
  if (kinds()[2].available) {
    lzipMembersAndTrailingData(folder);
  }
  for (const auto& kind : kinds()) {
    if (!kind.available) {
      continue;
    }
    browsesAndUnpacks(folder, kind);
    brokenStreamsFail(folder, kind);
    decodedBombHitsTheLimit(folder, kind);
    if (kind.filter == ARCHIVE_FILTER_LZMA || kind.filter == ARCHIVE_FILTER_LZIP) {
      oversizedDictionaryIsRefused(folder, kind);
    }
  }
  std::println("archive-tar-filters: all checks passed");
  return 0;
}
