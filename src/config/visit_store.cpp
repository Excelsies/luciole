#include "config/visit_store.h"

#include "config/state_store.h"
#include "fs/xdg_dirs.h"

#include <algorithm>
#include <charconv>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace {
  using luciole::Visit;
  using luciole::VisitStore;
  using Part = VisitStore::Part;

  constexpr std::string_view kHeaderPrefix = "luciole visits ";
  constexpr std::string_view kHeader = "luciole visits 1\n";
  constexpr std::size_t kMaxUriBytes = 4096;
  constexpr std::size_t kMaxPartBytes = 256 * 1024; // the private file limit
  constexpr std::int64_t kMaxTime = luciole::kMaxVisitTime;

  void merge(Visit& into, Visit from) { into = luciole::mergeVisits(into, from); }

  std::size_t lineBytes(const std::string& uri, Visit visit) {
    return std::formatted_size("{}\t{}\t{}\n", visit.count, visit.last, uri);
  }

  template <class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? std::optional<T>(value) : std::nullopt;
  }

  struct Damage {
    std::uint32_t line = 0;
    std::string message;
    bool otherVersion = false; // a well-formed header of another format version
  };

  Damage headerDamage(std::string_view text) {
    if (text.starts_with(kHeaderPrefix)) {
      const auto rest = text.substr(kHeaderPrefix.size());
      const auto end = rest.find('\n');
      const auto version = end == std::string_view::npos ? std::nullopt : number<std::uint32_t>(rest.substr(0, end));
      if (version && *version != 1) {
        return {
            .line = 1,
            .message = std::format("written in visit format {}, which this version does not read", *version),
            .otherVersion = true,
        };
      }
    }
    return {.line = 1, .message = "expected the header 'luciole visits 1'"};
  }

  std::optional<Damage> parseLine(std::string_view line, std::size_t part, Part& out) {
    const auto first = line.find('\t');
    const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
    if (second == std::string_view::npos) {
      return Damage{.message = "expected a count, a time and a URI separated by tabs"};
    }
    const auto count = number<std::uint32_t>(line.substr(0, first));
    const auto last = number<std::int64_t>(line.substr(first + 1, second - first - 1));
    if (!count || *count == 0) {
      return Damage{.message = "expected a count from 1 to 4294967295"};
    }
    if (!last || *last < 0 || *last > kMaxTime) {
      return Damage{.message = "expected a time in seconds from 0 to 253402300799"};
    }
    std::string uri(line.substr(second + 1));
    if (auto problem = luciole::visitUriProblem(uri)) {
      return Damage{.message = std::move(*problem)};
    }
    if (VisitStore::partOf(uri) != part) {
      return Damage{.message = "location belongs to another visit file"};
    }
    if (!out.emplace(std::move(uri), Visit{.count = *count, .last = *last}).second) {
      return Damage{.message = "location appears twice"};
    }
    return std::nullopt;
  }

  std::expected<Part, Damage> parsePart(std::string_view text, std::size_t part) {
    if (!text.starts_with(kHeader)) {
      return std::unexpected(headerDamage(text));
    }
    Part result;
    auto rest = text.substr(kHeader.size());
    // Each pass consumes one line, and a part may hold at most partLimit of them.
    for (std::uint32_t line = 2; !rest.empty(); ++line) {
      if (result.size() == VisitStore::partLimit) {
        return std::unexpected(Damage{.line = line, .message = "more than 625 locations"});
      }
      const auto end = rest.find('\n');
      if (end == std::string_view::npos) {
        return std::unexpected(Damage{.line = line, .message = "line ends without a newline"});
      }
      if (auto damage = parseLine(rest.substr(0, end), part, result)) {
        damage->line = line;
        return std::unexpected(std::move(*damage));
      }
      rest.remove_prefix(end + 1);
    }
    return result;
  }

  std::string formatPart(const Part& part) {
    std::string text(kHeader);
    for (const auto& [uri, visit] : part) {
      std::format_to(std::back_inserter(text), "{}\t{}\t{}\n", visit.count, visit.last, uri);
    }
    return text;
  }

  // Keeps the most recently visited locations that fit the part's count and file size.
  Part prune(Part part) {
    std::size_t bytes = kHeader.size();
    for (const auto& [uri, visit] : part) {
      bytes += lineBytes(uri, visit);
    }
    if (part.size() <= VisitStore::partLimit && bytes <= kMaxPartBytes) {
      return part;
    }
    std::vector<Part::const_iterator> order;
    order.reserve(part.size());
    for (auto item = part.cbegin(); item != part.cend(); ++item) {
      order.push_back(item);
    }
    std::ranges::sort(order, [](Part::const_iterator a, Part::const_iterator b) {
      if (a->second.last != b->second.last) {
        return a->second.last > b->second.last;
      }
      if (a->second.count != b->second.count) {
        return a->second.count > b->second.count;
      }
      return a->first < b->first;
    });
    Part kept;
    bytes = kHeader.size();
    for (const auto item : order) {
      bytes += lineBytes(item->first, item->second);
      if (kept.size() == VisitStore::partLimit || bytes > kMaxPartBytes) {
        break;
      }
      kept.emplace(item->first, item->second);
    }
    return kept;
  }

  luciole::ConfigIssue issue(const std::filesystem::path& file, std::string message, std::uint32_t line = 0) {
    return {.file = file, .line = line, .column = 0, .key = {}, .message = std::move(message)};
  }

  int severity(luciole::AtomicWriteState state) {
    switch (state) {
    case luciole::AtomicWriteState::Unchanged:
      return 0;
    case luciole::AtomicWriteState::Durable:
      return 1;
    case luciole::AtomicWriteState::Committed:
      return 2;
    case luciole::AtomicWriteState::NotCommitted:
      return 3;
    }
    return 3;
  }

  luciole::AtomicWriteState worse(luciole::AtomicWriteState a, luciole::AtomicWriteState b) {
    return severity(a) >= severity(b) ? a : b;
  }
} // namespace

luciole::Visit luciole::mergeVisits(Visit a, Visit b) {
  const auto room = std::numeric_limits<std::uint32_t>::max() - a.count;
  return {
      .count = b.count > room ? std::numeric_limits<std::uint32_t>::max() : a.count + b.count,
      .last = std::max(a.last, b.last),
  };
}

std::optional<std::string> luciole::visitUriProblem(const std::string& uri) {
  if (uri.size() > kMaxUriBytes) {
    return "expected a URI of at most 4096 bytes";
  }
  if (std::ranges::any_of(uri, [](unsigned char c) { return c < 32 || c == 127; })) {
    return "expected a URI without control characters";
  }
  return storedUriProblem(uri);
}

luciole::VisitStore::VisitStore(std::filesystem::path directory) : m_directory(std::move(directory)) {}

std::filesystem::path luciole::VisitStore::pathFromEnvironment() { return stateHome() / "luciole/visits"; }

std::size_t luciole::VisitStore::partOf(std::string_view uri) {
  // FNV-1a, so every process and version files a location in the same part.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto c : uri) {
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
  }
  return static_cast<std::size_t>(hash % partCount);
}

bool luciole::VisitStore::validDirectory() const {
  return m_directory.is_absolute() && !m_directory.native().contains('\0');
}

std::filesystem::path luciole::VisitStore::partPath(std::size_t part) const {
  return m_directory / std::format("{:02}.tsv", part);
}

void luciole::VisitStore::load() {
  m_issues.clear();
  m_parts = {};
  if (!validDirectory()) {
    m_issues.push_back(issue(m_directory, "expected an absolute visit store directory"));
    return;
  }
  for (std::size_t part = 0; part < partCount; ++part) {
    const auto path = partPath(part);
    const auto text = readPrivateTextFile(path);
    if (!text) {
      m_issues.push_back(issue(path, std::format("{}; its visits start empty", text.error())));
      continue;
    }
    if (!*text) {
      continue;
    }
    auto parsed = parsePart(**text, part);
    if (!parsed) {
      const auto& damage = parsed.error();
      m_issues.push_back(issue(
          path,
          damage.otherVersion ? std::format("visit file {}; its visits read as empty", damage.message)
                              : std::format("damaged visit file: {}; its visits start empty", damage.message),
          damage.line
      ));
      continue;
    }
    m_parts[part] = std::move(*parsed);
  }
}

luciole::AtomicWriteState
luciole::VisitStore::update(std::size_t part, std::string_view failure, const std::function<Part(Part)>& change) {
  const auto path = partPath(part);
  Part next;
  const auto result = updateTextFileAtomic(
      path, [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        Part base;
        if (current) {
          if (auto parsed = parsePart(*current, part)) {
            base = std::move(*parsed);
          } else if (parsed.error().otherVersion) {
            return std::unexpected(std::format("visit file {}; it is left unchanged", parsed.error().message));
          } else {
            m_issues.push_back(issue(
                path, std::format("replaced a damaged visit file: {}", parsed.error().message), parsed.error().line
            ));
          }
        }
        next = change(std::move(base));
        if (!current && next.empty()) {
          return std::nullopt;
        }
        return formatPart(next);
      }
  );
  if (result.state != AtomicWriteState::NotCommitted) {
    m_parts[part] = std::move(next);
  }
  if (!result.error.empty()) {
    m_issues.push_back(issue(path, std::format("{}: {}", failure, result.error)));
  }
  return result.state;
}

luciole::AtomicWriteState luciole::VisitStore::add(std::span<const VisitRecord> visits) {
  m_issues.clear();
  if (visits.empty()) {
    return AtomicWriteState::Unchanged;
  }
  if (!validDirectory()) {
    m_issues.push_back(issue(m_directory, "expected an absolute visit store directory"));
    return AtomicWriteState::NotCommitted;
  }
  Parts changes;
  for (const auto& record : visits) {
    auto problem = visitUriProblem(record.uri);
    if (!problem && (record.visit.count == 0 || record.visit.last < 0 || record.visit.last > kMaxTime)) {
      problem = "expected a count of at least 1 and a time from 0 to 253402300799";
    }
    if (problem) {
      // The URI itself stays out of the issue: it may hold credentials.
      m_issues.push_back(issue(m_directory, std::format("refused a visit: {}", *problem)));
      return AtomicWriteState::NotCommitted;
    }
    merge(changes[partOf(record.uri)][record.uri], record.visit);
  }
  auto state = AtomicWriteState::Unchanged;
  for (std::size_t part = 0; part < partCount; ++part) {
    if (changes[part].empty()) {
      continue;
    }
    state = worse(state, update(part, "visits not saved", [&](Part current) {
                    for (const auto& [uri, visit] : changes[part]) {
                      merge(current[uri], visit);
                    }
                    return prune(std::move(current));
                  }));
  }
  return state;
}

luciole::AtomicWriteState luciole::VisitStore::clear() {
  m_issues.clear();
  if (!validDirectory()) {
    m_issues.push_back(issue(m_directory, "expected an absolute visit store directory"));
    return AtomicWriteState::NotCommitted;
  }
  auto state = AtomicWriteState::Unchanged;
  for (std::size_t part = 0; part < partCount; ++part) {
    std::error_code error;
    if (std::filesystem::symlink_status(partPath(part), error).type() == std::filesystem::file_type::not_found) {
      m_parts[part].clear();
      continue;
    }
    state = worse(state, update(part, "visits not cleared", [](const Part&) { return Part{}; }));
  }
  return state;
}
