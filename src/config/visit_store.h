#pragma once

#include "config/diagnostic.h"
#include "fs/atomic_file.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace luciole {
  // How often a location was entered or opened, and when last, in seconds since the Unix epoch.
  struct Visit {
    std::uint32_t count = 0;
    std::int64_t last = 0;
    bool operator==(const Visit&) const = default;
  };

  struct VisitRecord {
    std::string uri;
    Visit visit;
    bool operator==(const VisitRecord&) const = default;
  };

  // The latest visit time the store accepts: 9999-12-31T23:59:59Z.
  inline constexpr std::int64_t kMaxVisitTime = 253402300799;

  // The sum of both counts, saturating at the largest count, and the later of the two times.
  Visit mergeVisits(Visit a, Visit b);

  // Empty for a URI the visit store may keep: absolute, at most 4096 bytes, without control characters,
  // credentials, query or fragment.
  std::optional<std::string> visitUriProblem(const std::string& uri);

  // Private visit counts in 32 files under one directory, each location filed by a hash of its URI. A part keeps
  // at most 625 locations within the private file limit, so the store holds at most 20,000; a full part drops its
  // least recently visited locations. Single owning I/O thread.
  class VisitStore {
  public:
    static constexpr std::size_t partCount = 32;
    static constexpr std::size_t partLimit = 625;
    static constexpr std::size_t locationLimit = partCount * partLimit;
    using Part = std::map<std::string, Visit, std::less<>>;
    using Parts = std::array<Part, partCount>;

    explicit VisitStore(std::filesystem::path directory);
    static std::filesystem::path pathFromEnvironment();
    static std::size_t partOf(std::string_view uri);
    // Reads every part. A part that cannot be read, is damaged or is in another format version reads as empty and
    // adds an issue.
    void load();
    // Adds each record's count to its location and keeps the later time, merged under the file lock with what
    // other writers saved. A record with no count or a URI the store may not keep refuses the whole batch. A damaged
    // part is replaced; a part that cannot be read or is in another format version is left as it is and reported.
    AtomicWriteState add(std::span<const VisitRecord> visits);
    // Rewrites every existing part without locations. A part that cannot be rewritten keeps its visits, here and on
    // disk, and is reported as not cleared.
    AtomicWriteState clear();
    const Parts& parts() const { return m_parts; }
    // Problems from the last load, add or clear.
    const ConfigIssues& issues() const { return m_issues; }

  private:
    bool validDirectory() const;
    std::filesystem::path partPath(std::size_t part) const;
    // `failure` starts the issue added when the part is not written.
    AtomicWriteState update(std::size_t part, std::string_view failure, const std::function<Part(Part)>& change);

    std::filesystem::path m_directory;
    Parts m_parts;
    ConfigIssues m_issues;
  };
} // namespace luciole
