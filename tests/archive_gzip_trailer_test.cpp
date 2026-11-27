#include "archive_format_harness.h"
#include "fs/archive_extra_types.h"

#include <archive.h>
#include <cstdlib>
#include <print>
#include <string>
#include <vector>

// gzip, bzip2 and zstd streams under a TAR: the TAR ends before the stream does, so damage to the trailer, a cut
// trailer and corrupt data that still inflates must be refused for inspect and extract alike. Set
// LUCIOLE_GZIP_TABLE to print one line per case.
namespace {
  using namespace luciole::test::formats;

  constexpr std::size_t kEdge = 64U * 1024U;
  constexpr std::string_view kMarker = "MARKER-LINE-OF-DATA-";
  int mismatches = 0;

  // A file whose bytes stay readable inside a stored deflate stream, so damage there cannot break the syntax.
  std::string content() {
    std::string bytes;
    for (int line = 0; bytes.size() < 30000; ++line) {
      bytes += std::string(kMarker) + std::to_string(line) + "\n";
    }
    return bytes;
  }
  std::string pseudoRandom(std::size_t size) {
    std::string bytes(size, '\0');
    std::uint32_t state = 777;
    for (auto& byte : bytes) {
      state = state * 1664525U + 1013904223U;
      byte = static_cast<char>(state >> 24);
    }
    return bytes;
  }
  std::string sampleTar() { return plainTar({{"data.txt", content()}, {"other.txt", "tail\n"}}); }

  std::string compressed(const Folder& folder, int filter, std::string_view bytes, std::optional<int> level = {}) {
    const auto path = folder.root / "stream.tmp";
    compressStream(path, filter, bytes, level);
    return read(path);
  }
  std::string flipped(std::string bytes, std::size_t at) {
    TEST_CHECK(at < bytes.size());
    bytes[at] = static_cast<char>(bytes[at] ^ 0x01);
    return bytes;
  }
  std::string cut(const std::string& bytes, std::size_t count) {
    TEST_CHECK(bytes.size() > count);
    return bytes.substr(0, bytes.size() - count);
  }
  std::size_t insideContent(const std::string& bytes) {
    const auto at = bytes.find(kMarker);
    TEST_CHECK(at != std::string::npos);
    return at + 40;
  }

  struct Subject {
    std::string name;     // the archive's file name
    std::string produced; // the file extraction must produce, relative to the destination
    std::string expected; // its bytes
  };
  int counter = 0;
  constexpr std::string_view kInside = "A gzip stream inside another compression cannot be verified.";

  // Inspects and extracts `bytes`; the case is right when `refused` matches, and an accepted case must extract the
  // exact bytes. A refusal must carry `message` when one is given.
  void check(
      const Folder& folder, const Subject& subject, const std::string& label, const std::string& bytes, bool refused,
      std::string_view message = {}
  ) {
    const auto dir = folder.root / ("case-" + std::to_string(counter++));
    fs::create_directory(dir);
    const auto path = dir / subject.name;
    write(path, bytes);
    const auto found = inspect(path);
    const auto out = dir / "out";
    const auto done = extract(path, out);
    const bool inspectRefused = found.error.has_value();
    const bool extractRefused = done.error.has_value();
    const bool wrong = !extractRefused && read(out / subject.produced) != subject.expected;
    if (std::getenv("LUCIOLE_GZIP_TABLE")) {
      std::println(
          "{} | {} | inspect {} | extract {} | {}", subject.name, label,
          inspectRefused ? "refused: " + found.error->message : "accepted",
          extractRefused ? "refused: " + done.error->message : "accepted", wrong ? "WRONG BYTES" : "bytes ok"
      );
    }
    const bool good = inspectRefused == refused
        && extractRefused == refused
        && !wrong
        && (message.empty() || (found.error && found.error->message.contains(message)))
        && (message.empty() || (done.error && done.error->message.contains(message)))
        && (!refused || (!found.catalog && !fs::exists(out) && tmpIsEmpty(folder)));
    if (!good) {
      std::println(stderr, "unexpected: {} {} (expected {})", subject.name, label, refused ? "refused" : "accepted");
      ++mismatches;
    }
  }

  // The same damage to a stream: trailer bytes, cuts, junk, data that inflates to something else.
  void damage(const Folder& folder, const Subject& subject, const std::string& good, int filter) {
    const auto size = good.size();
    check(folder, subject, "valid", good, false);
    if (filter == ARCHIVE_FILTER_GZIP) {
      check(folder, subject, "crc flipped", flipped(good, size - 8), true);
      check(folder, subject, "length flipped", flipped(good, size - 2), true);
      for (const std::size_t count : {1U, 4U, 8U}) {
        check(folder, subject, "cut " + std::to_string(count), cut(good, count), true);
      }
      check(folder, subject, "junk after member", good + "junk", true);
    } else {
      check(folder, subject, "cut 1", cut(good, 1), true);
      check(folder, subject, "trailer flipped", flipped(good, size - 3), true);
    }
  }

  void gzipStreams(const Folder& folder) {
    const Subject tar{"pack.tar.gz", "data.txt", content()};
    const Subject single{"notes.txt.gz", "notes.txt", content()};
    for (const int level : {6, 0}) {
      const auto suffix = " level " + std::to_string(level);
      const auto asTar = compressed(folder, ARCHIVE_FILTER_GZIP, sampleTar(), level);
      const auto asSingle = compressed(folder, ARCHIVE_FILTER_GZIP, content(), level);
      damage(folder, tar, asTar, ARCHIVE_FILTER_GZIP);
      damage(folder, single, asSingle, ARCHIVE_FILTER_GZIP);
      if (level == 0) {
        check(folder, tar, "data flipped" + suffix, flipped(asTar, insideContent(asTar)), true);
        check(folder, single, "data flipped" + suffix, flipped(asSingle, insideContent(asSingle)), true);
      }
    }
    check(
        folder, Subject{"pack.tgz", "data.txt", content()}, "valid",
        compressed(folder, ARCHIVE_FILTER_GZIP, sampleTar()), false
    );
  }

  // One TAR in two gzip members, as tools that compress in blocks write it.
  void gzipMembers(const Folder& folder) {
    const Subject tar{"pack.tar.gz", "data.txt", content()};
    const auto whole = sampleTar();
    const auto first = compressed(folder, ARCHIVE_FILTER_GZIP, whole.substr(0, 4096));
    const auto second = compressed(folder, ARCHIVE_FILTER_GZIP, whole.substr(4096));
    check(folder, tar, "two members valid", first + second, false);
    check(folder, tar, "two members, first crc flipped", flipped(first, first.size() - 8) + second, true);
    check(folder, tar, "two members, first length flipped", flipped(first, first.size() - 2) + second, true);
    check(folder, tar, "two members, second crc flipped", first + flipped(second, second.size() - 8), true);
    check(folder, tar, "two members, second cut 1", first + cut(second, 1), true);
    check(folder, tar, "two members, first cut 4", cut(first, 4) + second, true);
    const auto small = plainTar({{"data.txt", content()}});
    const auto a = compressed(folder, ARCHIVE_FILTER_GZIP, small.substr(0, 2048), 0);
    const auto b = compressed(folder, ARCHIVE_FILTER_GZIP, small.substr(2048), 0);
    check(folder, tar, "two members stored valid", a + b, false);
    check(folder, tar, "two members stored, data flipped in first", flipped(a, insideContent(a)) + b, true);
  }

  void limitsAndShapes(const Folder& folder) {
    const Subject tar{"pack.tar.gz", "data.txt", content()};
    const auto good = compressed(folder, ARCHIVE_FILTER_GZIP, sampleTar());
    const Subject single{"notes.txt.gz", "notes.txt", content()};
    const auto alone = compressed(folder, ARCHIVE_FILTER_GZIP, content());
    // NUL padding up to the end of the file is accepted, as gzip and libarchive do; nothing else after the end is.
    for (const auto& [subject, stream] : {std::pair{tar, good}, std::pair{single, alone}}) {
      for (const std::size_t zeros : {1U, 512U, 1U << 20}) {
        check(folder, subject, std::to_string(zeros) + " NULs after member", stream + std::string(zeros, '\0'), false);
      }
      check(folder, subject, "NULs then one byte", stream + std::string(512, '\0') + "x", true);
      check(folder, subject, "NULs then a member", stream + std::string(512, '\0') + stream, true);
      check(folder, subject, "one non-NUL byte", stream + "x", true);
    }
    check(folder, tar, "another gzip around it", compressed(folder, ARCHIVE_FILTER_GZIP, good), true);
    const auto twice = compressed(folder, ARCHIVE_FILTER_GZIP, good);
    check(folder, Subject{"x.tar.gz.gz", "x.tar.gz", good}, "gz of a .tar.gz", twice, true, kInside);
    check(folder, Subject{"x.gz", "x", good}, "single gz of a gzip", twice, true, kInside);
    // The TAR ends in the first bytes; the zeros after it still count against the decoded-container budget.
    const auto path = folder.root / "bomb.tar.gz";
    write(path, compressed(folder, ARCHIVE_FILTER_GZIP, plainTar({{"file", "abc"}}) + std::string(4U << 20, '\0')));
    luciole::ArchiveLimits limits;
    limits.maxExpandedBytes = 128;
    limits.maxMetadataBytes = 256;
    limits.maxEntries = 4;
    const auto found = inspect(path, limits);
    TEST_CHECK(found.error && !found.catalog && found.error->message.contains("decoded-container"));
    const auto out = folder.root / "bomb-out";
    const auto done = extract(path, out, {}, limits);
    TEST_CHECK(done.error && done.error->message.contains("decoded-container") && !fs::exists(out));
    TEST_CHECK(tmpIsEmpty(folder));
  }

  void bigFilesStayExact(const Folder& folder) {
    const auto big = pseudoRandom(6U * 1024U * 1024U);
    const Subject subject{"big.tar.gz", "big.bin", big};
    check(folder, subject, "big valid", compressed(folder, ARCHIVE_FILTER_GZIP, plainTar({{"big.bin", big}})), false);
  }

  void otherFilters(const Folder& folder) {
    damage(
        folder, Subject{"pack.tar.bz2", "data.txt", content()}, compressed(folder, ARCHIVE_FILTER_BZIP2, sampleTar()),
        ARCHIVE_FILTER_BZIP2
    );
    damage(
        folder, Subject{"pack.tar.zst", "data.txt", content()}, compressed(folder, ARCHIVE_FILTER_ZSTD, sampleTar()),
        ARCHIVE_FILTER_ZSTD
    );
  }

  // The file is one compression over another: the outer layer is the file's own, the inner one lies inside it.
  void stackedStreams(const Folder& folder) {
    const auto tarGz = compressed(folder, ARCHIVE_FILTER_GZIP, sampleTar());
    if (luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Bzip2)) {
      const auto tarBz2 = compressed(folder, ARCHIVE_FILTER_BZIP2, sampleTar());
      const Subject wrapped{"x.tar.bz2.gz", "x.tar.bz2", sampleTar()};
      const auto good = compressed(folder, ARCHIVE_FILTER_GZIP, tarBz2);
      check(folder, wrapped, "bzip2 inside gzip, valid", good, false);
      check(folder, wrapped, "bzip2 inside gzip, crc flipped", flipped(good, good.size() - 8), true, "data check");
      check(folder, wrapped, "bzip2 inside gzip, length flipped", flipped(good, good.size() - 2), true, "corrupt");
      check(folder, wrapped, "bzip2 inside gzip, cut 4", cut(good, 4), true, "Truncated");
      check(
          folder, Subject{"x.tar.gz.bz2", "x.tar.gz", sampleTar()}, "gzip inside bzip2",
          compressed(folder, ARCHIVE_FILTER_BZIP2, tarGz), true, kInside
      );
    }
    if (luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Zstd)) {
      const auto tarZst = compressed(folder, ARCHIVE_FILTER_ZSTD, sampleTar());
      const Subject wrapped{"x.tar.zst.gz", "x.tar.zst", sampleTar()};
      const auto good = compressed(folder, ARCHIVE_FILTER_GZIP, tarZst);
      check(folder, wrapped, "zstd inside gzip, valid", good, false);
      check(folder, wrapped, "zstd inside gzip, crc flipped", flipped(good, good.size() - 8), true, "data check");
      check(
          folder, Subject{"x.tar.gz.zst", "x.tar.gz", sampleTar()}, "gzip inside zstd",
          compressed(folder, ARCHIVE_FILTER_ZSTD, tarGz), true, kInside
      );
    }
    // The fork's own outer kinds over a gzip. libarchive sees the gzip inside lz4 and compress, which is refused;
    // XzInput decodes xz outside libarchive with no gzip filter, so the gzip comes out as data and is never read
    // as a layer; lzma and lzip files are not opened with a gzip under them.
    struct Outer {
      std::string name;
      int filter;
      bool available;
      std::string_view message;
    };
    const std::vector<Outer> outers{
        {"x.tar.gz.lz4", ARCHIVE_FILTER_LZ4, luciole::archiveExtraNeedAvailable(luciole::ArchiveExtraNeed::Lz4),
         kInside},
        {"x.tar.gz.Z", ARCHIVE_FILTER_COMPRESS, luciole::archiveExtraNeedAvailable(luciole::ArchiveExtraNeed::Compress),
         kInside},
        {"x.tar.gz.lzma", ARCHIVE_FILTER_LZMA, luciole::archiveExtraNeedAvailable(luciole::ArchiveExtraNeed::LzmaAlone),
         "Unrecognized archive format"},
        {"x.tar.gz.lz", ARCHIVE_FILTER_LZIP, luciole::archiveExtraNeedAvailable(luciole::ArchiveExtraNeed::Lzip),
         "Unrecognized archive format"},
    };
    for (const auto& outer : outers) {
      if (outer.available) {
        check(
            folder, Subject{outer.name, "x.tar.gz", tarGz}, "gzip inside " + outer.name,
            compressed(folder, outer.filter, tarGz), true, outer.message
        );
      }
    }
    if (luciole::archiveCompressionAvailable(luciole::ArchiveCompression::Xz)) {
      check(
          folder, Subject{"x.tar.gz.xz", "x.tar.gz", tarGz}, "gzip inside xz comes out as data",
          compressed(folder, ARCHIVE_FILTER_XZ, tarGz), false
      );
    }
  }

  // Members and padding that meet the 64 KiB read chunk at its edge.
  void chunkEdges(const Folder& folder) {
    const auto big = plainTar({{"big.bin", pseudoRandom(100000)}});
    const Subject tar{"edge.tar.gz", "big.bin", pseudoRandom(100000)};
    std::size_t at = 0;
    std::string first;
    // Stored blocks make the size grow one byte per byte, so one prefix length lands on the edge.
    for (std::size_t length = 65400; length <= 65536 && at == 0; ++length) {
      first = compressed(folder, ARCHIVE_FILTER_GZIP, big.substr(0, length), 0);
      if (first.size() == kEdge) {
        at = length;
      }
    }
    TEST_CHECK(at != 0);
    const auto second = compressed(folder, ARCHIVE_FILTER_GZIP, big.substr(at), 0);
    check(folder, tar, "first member ends on the chunk edge", first + second, false);
    check(
        folder, tar, "member on the edge, crc flipped", flipped(first, first.size() - 8) + second, true, "data check"
    );
    check(folder, tar, "member on the edge, second crc flipped", first + flipped(second, second.size() - 8), true);
    // A member that ends just short of the edge, then NULs across it.
    const auto whole = compressed(folder, ARCHIVE_FILTER_GZIP, big, 0);
    const auto pad = kEdge - whole.size() % kEdge + 100;
    check(folder, tar, "NULs across the chunk edge", whole + std::string(pad, '\0'), false);
    check(folder, tar, "NULs across the chunk edge, then a byte", whole + std::string(pad, '\0') + "x", true);
    check(folder, tar, "NULs from the chunk edge", first + second + std::string(kEdge, '\0'), false);
  }
} // namespace

int main() {
  const Folder folder;
  gzipStreams(folder);
  gzipMembers(folder);
  limitsAndShapes(folder);
  bigFilesStayExact(folder);
  otherFilters(folder);
  stackedStreams(folder);
  chunkEdges(folder);
  TEST_CHECK(mismatches == 0);
  std::println("archive-gzip-trailer: all checks passed");
  return 0;
}
