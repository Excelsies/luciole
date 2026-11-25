#include "ui/gtk_colors.h"

#include "fs/posix_fd.h"
#include "fs/xdg_dirs.h"
#include "ui/gtk_import.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  namespace fs = std::filesystem;
  using luciole::Fd;
  using luciole::importedPath;
  using luciole::importTarget;
  using luciole::trimSpace;
  constexpr int kReadRetries = 8;

  bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-'; }
  bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

  // A value without a trailing `!important`, which a definition may carry.
  std::string definedValue(std::string_view raw) {
    auto value = trimSpace(raw);
    constexpr std::string_view kImportant = "important";
    if (value.size() >= kImportant.size()
        && std::ranges::equal(value.substr(value.size() - kImportant.size()), kImportant, [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) == b;
           })) {
      auto rest = trimSpace(value.substr(0, value.size() - kImportant.size()));
      if (rest.ends_with('!')) {
        value = trimSpace(rest.substr(0, rest.size() - 1));
      }
    }
    return std::string(value);
  }

  // The index after the string that starts at `from` (a quote): at its closing quote or at the line's end.
  std::size_t afterString(std::string_view text, std::size_t from) {
    const char quote = text[from];
    for (std::size_t i = from + 1; i < text.size(); ++i) {
      if (text[i] == '\\') {
        ++i;
      } else if (text[i] == quote) {
        return i + 1;
      } else if (text[i] == '\n') {
        return i;
      }
    }
    return text.size();
  }

  bool startsUrl(std::string_view text, std::size_t at) {
    constexpr std::string_view kUrl = "url(";
    return (at == 0 || !isNameChar(text[at - 1]))
        && at + kUrl.size() <= text.size()
        && std::ranges::equal(text.substr(at, kUrl.size()), kUrl, [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) == b;
           });
  }

  // The index after the `url(...)` that starts at `from`.
  std::size_t afterUrl(std::string_view text, std::size_t from) {
    std::size_t i = from + std::string_view("url(").size();
    while (i < text.size() && text[i] != ')') {
      i = text[i] == '"' || text[i] == '\'' ? afterString(text, i) : i + 1;
    }
    return std::min(i + 1, text.size());
  }

  // `--accent-bg-color` and `accent_bg_color` are one name.
  std::string normalized(std::string_view name) {
    std::string result(name);
    std::ranges::replace(result, '-', '_');
    return result;
  }

  std::optional<Color> literalColor(std::string_view value) {
    const bool sized = value.size() == 4 || value.size() == 7 || value.size() == 9;
    if (!sized || value.front() != '#' || !std::ranges::all_of(value.substr(1), [](char c) {
          return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      return std::nullopt;
    }
    return hex(value);
  }

  // The name `@name` or `var(--name)` refers to, or nullopt for any other value.
  std::optional<std::string> referenceName(std::string_view value) {
    if (value.starts_with('@')) {
      const auto name = value.substr(1);
      return !name.empty() && std::ranges::all_of(name, isNameChar) ? std::optional(normalized(name)) : std::nullopt;
    }
    if (value.starts_with("var(") && value.ends_with(')')) {
      const auto name = trimSpace(value.substr(4, value.size() - 5));
      if (name.starts_with("--") && name.size() > 2 && std::ranges::all_of(name, isNameChar)) {
        return normalized(name.substr(2));
      }
    }
    return std::nullopt;
  }

  std::optional<Color> resolve(const std::map<std::string, std::string, std::less<>>& raw, std::string_view name) {
    auto found = raw.find(name);
    // Each pass follows one reference; the chain ends at a color, at a name without one, or at the bound.
    for (int hops = 0; found != raw.end() && hops <= luciole::kGtkMaxReferenceDepth; ++hops) {
      if (const auto color = literalColor(found->second)) {
        return color;
      }
      const auto next = referenceName(found->second);
      if (!next) {
        return std::nullopt;
      }
      found = raw.find(*next);
    }
    return std::nullopt;
  }

  // One reading: the files seen, the raw value of every name, in document order.
  class Reading {
  public:
    void file(const fs::path& path, int depth) {
      auto where = path.lexically_normal();
      if (m_seen.size() >= luciole::kGtkMaxFiles || !m_seen.insert(where).second) {
        return;
      }
      m_tried.push_back(where);
      const auto text = luciole::readGtkFile(where);
      if (!text) {
        return;
      }
      m_files.push_back(where);
      scan(*text, where, depth);
    }

    luciole::GtkColors finish() && {
      luciole::GtkColors result;
      for (const auto& entry : m_raw) {
        if (const auto color = resolve(m_raw, entry.first)) {
          result.colors.emplace(entry.first, *color);
        }
      }
      result.files = std::move(m_files);
      result.tried = std::move(m_tried);
      return result;
    }

  private:
    struct Statement {
      std::string value;
      std::size_t end;
      bool terminated;
    };

    // Reads from `from` to the `;` that ends the statement, or to the `}` that closes its block (left in place),
    // without comments; parentheses and strings keep a `;` inside them from ending it.
    static Statement statement(std::string_view text, std::size_t from) {
      Statement result{.value = {}, .end = text.size(), .terminated = false};
      int parens = 0;
      char quote = 0;
      for (std::size_t i = from; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
          result.value += c;
          if (c == '\\' && i + 1 < text.size()) {
            result.value += text[++i];
          } else if (c == quote) {
            quote = 0;
          }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
          const auto close = text.find("*/", i + 2);
          i = close == std::string_view::npos ? text.size() : close + 1;
        } else if (c == '"' || c == '\'') {
          quote = c;
          result.value += c;
        } else if (c == '(') {
          ++parens;
          result.value += c;
        } else if (c == ')') {
          parens = std::max(0, parens - 1);
          result.value += c;
        } else if (parens == 0 && (c == ';' || c == '}')) {
          result.end = c == ';' ? i + 1 : i;
          result.terminated = true;
          break;
        } else {
          result.value += c;
        }
      }
      return result;
    }

    static bool keyword(std::string_view text, std::size_t at, std::string_view word) {
      return text.substr(at).starts_with(word)
          && (at + word.size() >= text.size() || !isNameChar(text[at + word.size()]));
    }

    void define(std::string_view text, std::size_t& i) {
      i += std::string_view("@define-color").size();
      while (i < text.size() && isSpace(text[i])) {
        ++i;
      }
      const auto start = i;
      while (i < text.size() && isNameChar(text[i])) {
        ++i;
      }
      if (i == start) {
        return;
      }
      const auto name = normalized(text.substr(start, i - start));
      const auto body = statement(text, i);
      i = body.end;
      if (body.terminated) {
        m_raw[name] = definedValue(body.value);
      }
    }

    // `--name: value`; anything else leaves i past the dashes and name.
    void custom(std::string_view text, std::size_t& i) {
      const auto start = i + 2;
      auto nameEnd = start;
      while (nameEnd < text.size() && isNameChar(text[nameEnd])) {
        ++nameEnd;
      }
      i = nameEnd;
      if (nameEnd == start) {
        return;
      }
      auto at = nameEnd;
      while (at < text.size() && isSpace(text[at])) {
        ++at;
      }
      if (at >= text.size() || text[at] != ':') {
        return;
      }
      const auto body = statement(text, at + 1);
      i = body.end;
      if (body.terminated) {
        m_raw[normalized(text.substr(start, nameEnd - start))] = definedValue(body.value);
      }
    }

    void import(std::string_view text, std::size_t& i, const fs::path& importer, int depth) {
      i += std::string_view("@import").size();
      const auto body = statement(text, i);
      i = body.end;
      if (!body.terminated || depth >= luciole::kGtkMaxImportDepth) {
        return;
      }
      if (const auto path = importedPath(importTarget(body.value), importer)) {
        file(*path, depth + 1);
      }
    }

    void scan(std::string_view text, const fs::path& path, int depth) {
      std::size_t i = 0;
      // Each pass moves i forward by at least one byte, so a file of at most kGtkMaxFileBytes ends it.
      while (i < text.size()) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
          const auto close = text.find("*/", i + 2);
          i = close == std::string_view::npos ? text.size() : close + 2;
        } else if (c == '"' || c == '\'') {
          i = afterString(text, i);
        } else if (startsUrl(text, i)) {
          i = afterUrl(text, i);
        } else if (c == '@' && keyword(text, i + 1, "define-color")) {
          define(text, i);
        } else if (c == '@' && keyword(text, i + 1, "import")) {
          import(text, i, path, depth);
        } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '-' && (i == 0 || !isNameChar(text[i - 1]))) {
          custom(text, i);
        } else {
          ++i;
        }
      }
    }

    std::set<fs::path> m_seen;
    std::vector<fs::path> m_tried;
    std::vector<fs::path> m_files;
    std::map<std::string, std::string, std::less<>> m_raw;
  };
} // namespace

std::optional<std::string> luciole::readGtkFile(const fs::path& path) {
  // Opened without blocking and checked on the descriptor, so a path that is swapped for a pipe cannot hold the
  // caller and the size checked is the size read.
  const Fd fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  struct stat info{};
  if (!fd.valid()
      || ::fstat(fd.get(), &info) != 0
      || !S_ISREG(info.st_mode)
      || info.st_size < 0
      || static_cast<std::uintmax_t>(info.st_size) > kGtkMaxFileBytes) {
    return std::nullopt;
  }
  const auto size = static_cast<std::size_t>(info.st_size);
  std::string text(size, '\0');
  std::size_t have = 0;
  // Each pass reads at least one byte or ends; the bytes wanted are the size above. A signal may retry a few times.
  for (int interrupts = 0; have < size;) {
    const ssize_t got = ::read(fd.get(), text.data() + have, size - have);
    if (got == 0) {
      break;
    }
    if (got < 0) {
      if (errno == EINTR && ++interrupts < kReadRetries) {
        continue;
      }
      return std::nullopt;
    }
    have += static_cast<std::size_t>(got);
  }
  return have == size ? std::optional(std::move(text)) : std::nullopt;
}

luciole::GtkColors luciole::readGtkColors(const fs::path& root) {
  Reading reading;
  reading.file(root, 0);
  return std::move(reading).finish();
}

fs::path luciole::gtkCssPath() { return configHome() / "gtk-4.0" / "gtk.css"; }
