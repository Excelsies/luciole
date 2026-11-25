#pragma once

#include "event_loop/glib_poll_source.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "operations/archive_service.h"
#include "temp_folder.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

// What the archive-format tests share: a private folder, the service pump, and fixtures written with libarchive's
// writer.
namespace luciole::test::formats {
  namespace fs = std::filesystem;

  struct Folder {
    TempFolder tempFolder{"luciole-archive-formats-test", "/tmp"};
    fs::path root = tempFolder.path();
    Folder() {
      fs::create_directory(root / "tmp");
      ::setenv("TMPDIR", (root / "tmp").c_str(), 1);
    }
  };

  // Caps the address space at what the process uses now plus `headroom`, so a decoder that allocated its declared
  // dictionary would fail where the limit check must have refused it first.
  class AddressCap {
  public:
    explicit AddressCap(std::size_t headroom) {
      TEST_CHECK(::getrlimit(RLIMIT_AS, &m_before) == 0);
      std::ifstream statm("/proc/self/statm");
      std::size_t pages = 0;
      statm >> pages;
      TEST_CHECK(pages > 0);
      rlimit capped = m_before;
      capped.rlim_cur = pages * static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)) + headroom;
      TEST_CHECK(::setrlimit(RLIMIT_AS, &capped) == 0);
    }
    ~AddressCap() { ::setrlimit(RLIMIT_AS, &m_before); }
    AddressCap(const AddressCap&) = delete;
    AddressCap& operator=(const AddressCap&) = delete;

  private:
    rlimit m_before{};
  };

  inline bool tmpIsEmpty(const Folder& folder) { return fs::is_empty(folder.root / "tmp"); }

  inline void waitFor(ArchiveService& service) {
    using namespace std::chrono_literals;
    GlibPollSource source;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (service.busy()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      std::vector<pollfd> fds;
      auto start = source.addPollFds(fds);
      const int timeout = source.pollTimeoutMs();
      TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
      source.dispatch(fds, start);
    }
  }
  inline std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  inline void write(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(out.good());
  }
  inline Location location(const fs::path& path) { return Location::fromPath(path.native()); }

  struct Member {
    std::string name, data;
    int type;
    std::string link; // symlink target, or hard-link target
    bool hard;        // `link` names a hard-link target
    Member(std::string n, std::string d = {}, int t = AE_IFREG, std::string l = {}, bool h = false)
        : name(std::move(n)), data(std::move(d)), type(t), link(std::move(l)), hard(h) {}
  };

  using Writer = std::unique_ptr<struct archive, decltype(&archive_write_free)>;
  inline Writer newWriter() { return {archive_write_new(), &archive_write_free}; }

  inline void addMember(struct archive* writer, const Member& member) {
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), member.name.c_str());
    archive_entry_set_filetype(entry.get(), static_cast<unsigned>(member.type));
    archive_entry_set_perm(entry.get(), member.type == AE_IFDIR ? 0755 : 0644);
    if (member.type == AE_IFLNK) {
      archive_entry_set_symlink(entry.get(), member.link.c_str());
    } else if (member.hard) {
      archive_entry_set_hardlink(entry.get(), member.link.c_str());
    }
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(member.data.size()));
    TEST_CHECK(archive_write_header(writer, entry.get()) == ARCHIVE_OK);
    if (!member.data.empty()) {
      TEST_CHECK(
          archive_write_data(writer, member.data.data(), member.data.size())
          == static_cast<la_ssize_t>(member.data.size())
      );
    }
  }

  // The bytes of a plain TAR holding `members`.
  inline std::string plainTar(const std::vector<Member>& members) {
    std::size_t room = 1U << 20;
    for (const auto& member : members) {
      room += member.data.size() + member.name.size() + member.link.size() + 2048;
    }
    std::string bytes(room, '\0');
    std::size_t used = 0;
    const auto writer = newWriter();
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter_none(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_memory(writer.get(), bytes.data(), bytes.size(), &used) == ARCHIVE_OK);
    for (const auto& member : members) {
      addMember(writer.get(), member);
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    bytes.resize(used);
    return bytes;
  }

  // `bytes` as one stream through the writer's filter `code`.
  inline void compressStream(const fs::path& path, int code, std::string_view bytes) {
    const auto writer = newWriter();
    TEST_CHECK(archive_write_set_format_raw(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter(writer.get(), code) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "data");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(bytes.size()));
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }

  // What one inspect and one extract of `source` end with.
  struct Outcome {
    std::optional<FileError> error;
    std::shared_ptr<const ArchiveCatalog> catalog;
  };
  inline Outcome inspect(const fs::path& source, ArchiveLimits limits = {}) {
    ArchiveService service(limits);
    TEST_CHECK(service.inspect(location(source)));
    waitFor(service);
    return {service.error(), service.catalog()};
  }
  inline Outcome extract(
      const fs::path& source, const fs::path& destination, std::vector<std::string> selected = {},
      ArchiveLimits limits = {}
  ) {
    ArchiveService service(limits);
    TEST_CHECK(service.extract(location(source), location(destination), std::move(selected), nullptr));
    waitFor(service);
    return {service.error(), service.catalog()};
  }
  inline bool hasEntry(const Outcome& outcome, std::string_view path) {
    return outcome.catalog
        && std::ranges::any_of(outcome.catalog->entries, [&](const auto& entry) { return entry.path == path; });
  }
} // namespace luciole::test::formats
