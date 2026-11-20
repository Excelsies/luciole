#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace luciole {
  // Bounds on what readZipMembers holds in memory. Every member counts toward maxMembers, kept or not.
  struct ZipMemberLimits {
    std::size_t maxMemberBytes = 64U * 1024U * 1024U;
    std::size_t maxTotalBytes = 160U * 1024U * 1024U;
    std::size_t maxMembers = 10'000;
  };

  enum class ZipMemberError : std::uint8_t {
    Unreadable,     // not a ZIP archive, damaged, encrypted or cut short
    TooManyMembers, // more than maxMembers
    MemberTooLarge, // a kept member unpacks to more than maxMemberBytes
    TotalTooLarge,  // the kept members unpack to more than maxTotalBytes
    DuplicateName,  // two kept members share a name
  };

  // Archive members held in memory, by name.
  class ZipMembers {
  public:
    // The member's bytes; empty for a name the archive did not hold or the reader did not keep.
    const std::string* find(std::string_view name) const;
    std::size_t size() const noexcept { return m_members.size(); }

  private:
    friend std::expected<ZipMembers, ZipMemberError>
    readZipMembers(int fd, const std::function<bool(std::string_view)>& wanted, const ZipMemberLimits& limits);

    std::map<std::string, std::string, std::less<>> m_members;
  };

  // Reads the regular file members of the ZIP archive open at `fd` whose names `wanted` accepts. Sizes the archive
  // states are not trusted: unpacked bytes are counted as they arrive. Blocking; the archive is untrusted, so run it
  // only in a resource-limited helper. The descriptor stays open.
  std::expected<ZipMembers, ZipMemberError>
  readZipMembers(int fd, const std::function<bool(std::string_view)>& wanted, const ZipMemberLimits& limits = {});
} // namespace luciole
