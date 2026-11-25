#include "ui/gtk_colors.h"

#include "fs/xdg_dirs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string_view>
#include <utility>

namespace {
  namespace fs = std::filesystem;
  using luciole::GtkColorTable;

  bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-'; }
  bool isSpace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

  std::string_view trim(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
      text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
      text.remove_suffix(1);
    }
    return text;
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
      const auto name = trim(value.substr(4, value.size() - 5));
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

  int hexValue(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0 ? c - '0' : (std::tolower(c) - 'a') + 10;
  }

  std::optional<std::string> percentDecoded(std::string_view text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '%') {
        result += text[i];
        continue;
      }
      if (i + 2 >= text.size()) {
        return std::nullopt;
      }
      if (std::isxdigit(static_cast<unsigned char>(text[i + 1])) == 0
          || std::isxdigit(static_cast<unsigned char>(text[i + 2])) == 0) {
        return std::nullopt;
      }
      result += static_cast<char>((hexValue(text[i + 1]) * 16) + hexValue(text[i + 2]));
      i += 2;
    }
    return !result.contains('\0') ? std::optional(result) : std::nullopt;
  }

  // The local file an import names, or nullopt for another scheme or an unusable name.
  std::optional<fs::path> importedPath(std::string_view target, const fs::path& importer) {
    target = trim(target);
    if (target.empty()) {
      return std::nullopt;
    }
    const auto colon = target.find(':');
    const bool hasScheme =
        colon != std::string_view::npos
        && std::isalpha(static_cast<unsigned char>(target[0])) != 0
        && std::ranges::all_of(target.substr(0, colon), [](char c) {
             return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
           });
    if (hasScheme) {
      constexpr std::string_view kFile = "file:///";
      if (target.size() < kFile.size()
          || !std::ranges::equal(target.substr(0, kFile.size()), kFile, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
             })) {
        return std::nullopt;
      }
      const auto decoded = percentDecoded(target.substr(kFile.size() - 1));
      return decoded ? std::optional<fs::path>(*decoded) : std::nullopt;
    }
    const fs::path path{std::string(target)};
    return path.is_absolute() ? path : importer.parent_path() / path;
  }

  // The target of `url("x")`, `url('x')`, `url(x)` or `"x"`; empty for anything else.
  std::string_view importTarget(std::string_view value) {
    value = trim(value);
    const bool inUrl = value.starts_with("url(");
    if (inUrl) {
      value = trim(value.substr(4));
    }
    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
      const auto close = value.find(value.front(), 1);
      return close == std::string_view::npos ? std::string_view{} : value.substr(1, close - 1);
    }
    const auto close = value.find(')');
    return inUrl && close != std::string_view::npos ? trim(value.substr(0, close)) : std::string_view{};
  }

  // One reading: the files seen, the raw value of every name, in document order.
  class Reading {
  public:
    explicit Reading(const luciole::GtkFileReader& read) : m_read(read) {}

    void file(const fs::path& path, int depth) {
      auto where = path.lexically_normal();
      if (m_seen.size() >= luciole::kGtkMaxFiles || !m_seen.insert(where).second) {
        return;
      }
      const auto text = m_read(where);
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
        m_raw[name] = std::string(trim(body.value));
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
        m_raw[normalized(text.substr(start, nameEnd - start))] = std::string(trim(body.value));
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

    const luciole::GtkFileReader& m_read;
    std::set<fs::path> m_seen;
    std::vector<fs::path> m_files;
    std::map<std::string, std::string, std::less<>> m_raw;
  };
} // namespace

std::optional<std::string> luciole::readGtkFile(const fs::path& path) {
  std::error_code error;
  if (!fs::is_regular_file(path, error) || error) {
    return std::nullopt;
  }
  const auto size = fs::file_size(path, error);
  if (error || size > kGtkMaxFileBytes) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  std::string text(size, '\0');
  if (!file.read(text.data(), static_cast<std::streamsize>(size)) || static_cast<std::size_t>(file.gcount()) != size) {
    return std::nullopt;
  }
  return text;
}

luciole::GtkColors luciole::readGtkColors(const fs::path& root, const GtkFileReader& read) {
  Reading reading(read);
  reading.file(root, 0);
  return std::move(reading).finish();
}

fs::path luciole::gtkCssPath() { return configHome() / "gtk-4.0" / "gtk.css"; }
