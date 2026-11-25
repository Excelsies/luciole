#include "ui/gtk_import.h"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

std::string_view luciole::trimSpace(std::string_view text) {
  const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!text.empty() && space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

namespace {
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

} // namespace

// The local file an import names, or nullopt for another scheme or an unusable name.
std::optional<fs::path> luciole::importedPath(std::string_view target, const fs::path& importer) {
  target = trimSpace(target);
  if (target.empty()) {
    return std::nullopt;
  }
  const auto colon = target.find(':');
  const bool hasScheme = colon != std::string_view::npos
      && std::isalpha(static_cast<unsigned char>(target[0])) != 0
      && std::ranges::all_of(target.substr(0, colon), [](char c) {
                           return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
                         });
  if (hasScheme) {
    constexpr std::string_view kFile = "file:///";
    if (target.size() < kFile.size() || !std::ranges::equal(target.substr(0, kFile.size()), kFile, [](char a, char b) {
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
std::string_view luciole::importTarget(std::string_view value) {
  value = trimSpace(value);
  const bool inUrl = value.starts_with("url(");
  if (inUrl) {
    value = trimSpace(value.substr(4));
  }
  if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
    const auto close = value.find(value.front(), 1);
    return close == std::string_view::npos ? std::string_view{} : value.substr(1, close - 1);
  }
  const auto close = value.find(')');
  return inUrl && close != std::string_view::npos ? trimSpace(value.substr(0, close)) : std::string_view{};
}
