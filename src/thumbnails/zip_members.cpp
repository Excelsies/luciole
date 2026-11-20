#include "thumbnails/zip_members.h"

#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <memory>
#include <sys/stat.h>
#include <utility>

namespace {
  constexpr std::size_t kBlockBytes = 64U * 1024U;

  using ArchivePtr = std::unique_ptr<archive, decltype(&archive_read_free)>;

  // The data of the current entry, while it stays within both limits; `total` counts what all members kept.
  std::expected<std::string, luciole::ZipMemberError>
  readData(archive* input, std::size_t& total, const luciole::ZipMemberLimits& limits) {
    using luciole::ZipMemberError;
    std::string out;
    std::array<char, kBlockBytes> block{};
    // Each pass adds at least one byte or ends, so the member limit bounds the passes.
    for (std::size_t pass = 0; pass <= limits.maxMemberBytes; ++pass) {
      const la_ssize_t count = archive_read_data(input, block.data(), block.size());
      if (count == 0) {
        return out;
      }
      if (count < 0) {
        return std::unexpected(ZipMemberError::Unreadable);
      }
      const auto bytes = static_cast<std::size_t>(count);
      if (bytes > limits.maxMemberBytes - out.size()) {
        return std::unexpected(ZipMemberError::MemberTooLarge);
      }
      if (bytes > limits.maxTotalBytes - total) {
        return std::unexpected(ZipMemberError::TotalTooLarge);
      }
      out.append(block.data(), bytes);
      total += bytes;
    }
    return std::unexpected(ZipMemberError::MemberTooLarge);
  }
} // namespace

const std::string* luciole::ZipMembers::find(std::string_view name) const {
  const auto found = m_members.find(name);
  return found == m_members.end() ? nullptr : &found->second;
}

std::expected<luciole::ZipMembers, luciole::ZipMemberError>
luciole::readZipMembers(int fd, const std::function<bool(std::string_view)>& wanted, const ZipMemberLimits& limits) {
  struct stat info{};
  if (fd < 0 || ::fstat(fd, &info) < 0 || !S_ISREG(info.st_mode)) {
    return std::unexpected(ZipMemberError::Unreadable);
  }
  const ArchivePtr input(archive_read_new(), &archive_read_free);
  if (!input
      || archive_read_support_format_zip(input.get()) != ARCHIVE_OK
      || archive_read_open_fd(input.get(), fd, kBlockBytes) != ARCHIVE_OK) {
    return std::unexpected(ZipMemberError::Unreadable);
  }
  ZipMembers members;
  std::size_t total = 0;
  for (std::size_t count = 0; count <= limits.maxMembers; ++count) {
    archive_entry* entry = nullptr;
    const int status = archive_read_next_header(input.get(), &entry);
    if (status == ARCHIVE_EOF) {
      return members;
    }
    if (status < ARCHIVE_WARN || entry == nullptr) {
      return std::unexpected(ZipMemberError::Unreadable);
    }
    if (count == limits.maxMembers) {
      return std::unexpected(ZipMemberError::TooManyMembers);
    }
    const char* name = archive_entry_pathname(entry);
    if (archive_entry_filetype(entry) != AE_IFREG || name == nullptr || !wanted(name)) {
      continue;
    }
    if (archive_entry_size_is_set(entry) != 0 && std::cmp_greater(archive_entry_size(entry), limits.maxMemberBytes)) {
      return std::unexpected(ZipMemberError::MemberTooLarge);
    }
    auto bytes = readData(input.get(), total, limits);
    if (!bytes) {
      return std::unexpected(bytes.error());
    }
    if (!members.m_members.emplace(name, std::move(*bytes)).second) {
      return std::unexpected(ZipMemberError::DuplicateName);
    }
  }
  return std::unexpected(ZipMemberError::TooManyMembers);
}
