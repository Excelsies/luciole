#include "fs/posix_fd.h"
#include "table_fixture.h"
#include "test_check.h"
#include "thumbnails/zip_members.h"

#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole;
  using namespace luciole::test::table;

  bool xmlOnly(std::string_view name) { return name.ends_with(".xml") || name.ends_with(".rels"); }

  std::expected<ZipMembers, ZipMemberError>
  readPath(const std::filesystem::path& path, const ZipMemberLimits& limits = {}) {
    const Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    TEST_CHECK(file.valid());
    return readZipMembers(file.get(), xmlOnly, limits);
  }

  void keepsWantedMembers(const TemporaryFolder& folder) {
    const auto path = folder.path() / "book.zip";
    writeZip(
        path, {{"xl/", ""}, {"xl/workbook.xml", "<workbook/>"}, {"a.rels", "<r/>"}, {"pic.png", "PNG"}, {"e.xml", ""}}
    );
    const auto members = readPath(path);
    TEST_CHECK(members.has_value());
    TEST_CHECK(members->size() == 3);
    TEST_CHECK(members->find("xl/workbook.xml") != nullptr && *members->find("xl/workbook.xml") == "<workbook/>");
    TEST_CHECK(members->find("a.rels") != nullptr && *members->find("a.rels") == "<r/>");
    TEST_CHECK(members->find("e.xml") != nullptr && members->find("e.xml")->empty());
    TEST_CHECK(members->find("pic.png") == nullptr);
    TEST_CHECK(members->find("xl/") == nullptr);
    TEST_CHECK(members->find("missing.xml") == nullptr);
  }

  void refusesOtherFiles(const TemporaryFolder& folder) {
    const auto text = folder.path() / "plain.txt";
    writeFile(text, "this is not an archive at all");
    TEST_CHECK(readPath(text).error() == ZipMemberError::Unreadable);

    const auto empty = folder.path() / "empty.zip";
    writeFile(empty, "");
    TEST_CHECK(!readPath(empty).has_value());

    TEST_CHECK(readZipMembers(-1, xmlOnly).error() == ZipMemberError::Unreadable);
  }

  void refusesTruncatedArchives(const TemporaryFolder& folder) {
    const auto whole = folder.path() / "whole.zip";
    std::string big;
    for (int i = 0; i < 20'000; ++i) {
      big += std::format("<row n=\"{}\"/>", i * 7919);
    }
    writeZip(whole, {{"a.xml", big}, {"b.xml", big}});
    const auto size = std::filesystem::file_size(whole);
    const auto cut = folder.path() / "cut.zip";
    std::filesystem::copy_file(whole, cut);
    std::filesystem::resize_file(cut, size / 3);
    TEST_CHECK(!readPath(cut).has_value());
  }

  void boundsMembers(const TemporaryFolder& folder) {
    constexpr std::size_t kMiB = 1024 * 1024;
    const ZipMemberLimits limits{.maxMemberBytes = kMiB, .maxTotalBytes = 2 * kMiB, .maxMembers = 20};

    // Zeros deflate to almost nothing, so the declared and compressed sizes are no guide to what a member holds.
    const auto bomb = folder.path() / "bomb.zip";
    writeZip(bomb, {{"small.xml", "<a/>"}, {"zeros.xml", std::string(kMiB + 1, '\0')}});
    TEST_CHECK(std::filesystem::file_size(bomb) < 64 * 1024);
    TEST_CHECK(readPath(bomb, limits).error() == ZipMemberError::MemberTooLarge);

    // Without its central directory, a streamed member states no size before its data, so only the count of
    // unpacked bytes stops it.
    const auto streamed = folder.path() / "streamed.zip";
    writeZip(streamed, {{"zeros.xml", std::string(2 * kMiB, '\0')}}, false);
    std::string bytes(std::filesystem::file_size(streamed), '\0');
    {
      std::ifstream in(streamed, std::ios::binary);
      in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const auto directory = bytes.find("PK\x01\x02");
    TEST_CHECK(directory != std::string::npos);
    writeFile(streamed, std::string_view(bytes).substr(0, directory));
    TEST_CHECK(readPath(streamed, limits).error() == ZipMemberError::MemberTooLarge);

    const auto exact = folder.path() / "exact.zip";
    writeZip(exact, {{"zeros.xml", std::string(kMiB, '\0')}});
    const auto fits = readPath(exact, limits);
    TEST_CHECK(fits.has_value() && fits->find("zeros.xml")->size() == kMiB);

    const auto total = folder.path() / "total.zip";
    const std::string part(800 * 1024, 'x');
    writeZip(total, {{"1.xml", part}, {"2.xml", part}, {"3.xml", part}});
    TEST_CHECK(readPath(total, limits).error() == ZipMemberError::TotalTooLarge);

    // Members the reader does not keep still count.
    std::vector<ZipMember> many;
    many.reserve(21);
    for (int i = 0; i < 21; ++i) {
      many.emplace_back(std::format("image{}.png", i), "x");
    }
    const auto crowd = folder.path() / "crowd.zip";
    writeZip(crowd, many);
    TEST_CHECK(readPath(crowd, limits).error() == ZipMemberError::TooManyMembers);
    many.pop_back();
    writeZip(crowd, many);
    TEST_CHECK(readPath(crowd, limits).has_value());
  }

  void refusesRepeatedNames(const TemporaryFolder& folder) {
    const auto path = folder.path() / "twice.zip";
    writeZip(path, {{"xl/workbook.xml", "<one/>"}, {"xl/workbook.xml", "<two/>"}});
    TEST_CHECK(readPath(path).error() == ZipMemberError::DuplicateName);
  }

  // Each read starts at the archive's start whatever the descriptor's offset, so one descriptor serves several.
  void readsAgain(const TemporaryFolder& folder) {
    const auto path = folder.path() / "again.zip";
    writeZip(path, {{"a.xml", "<a/>"}, {"b.xml", "<b/>"}});
    const Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    TEST_CHECK(file.valid());
    for (const std::string_view name : {"b.xml", "a.xml", "b.xml"}) {
      const auto members = readZipMembers(file.get(), [&](std::string_view member) { return member == name; });
      TEST_CHECK(members.has_value() && members->size() == 1);
      TEST_CHECK(members->find(name) != nullptr && members->find(name)->size() == 4);
    }
  }
} // namespace

int main() {
  const TemporaryFolder folder;
  keepsWantedMembers(folder);
  refusesOtherFiles(folder);
  refusesTruncatedArchives(folder);
  boundsMembers(folder);
  refusesRepeatedNames(folder);
  readsAgain(folder);
  std::println("zip-members: all checks passed");
  return 0;
}
