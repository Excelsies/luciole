#include "formats/color_codes.h"

#include "formats/ascii.h"
#include "formats/color_notations.h"

#include <algorithm>
#include <optional>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kUrlBreaks = " \t\"'`()<>[]{},;";

    bool isWordByte(char c) { return isAsciiAlpha(c) || isAsciiDigit(c) || c == '_'; }

    std::uint8_t hexValue(char c) {
      const auto low = toAsciiLower(c);
      return static_cast<std::uint8_t>(isAsciiDigit(low) ? low - '0' : low - 'a' + 10);
    }

    bool endsIgnoringCase(std::string_view text, std::string_view lowercase) {
      return text.size() >= lowercase.size()
          && equalsIgnoringCase(text.substr(text.size() - lowercase.size()), lowercase);
    }

    bool cssLike(Language language) {
      return language == Language::Css || language == Language::Scss || language == Language::Html;
    }

    // Where in a line codes are looked for.
    enum class Scope : std::uint8_t { None, Line, Strings };

    Scope scopeOf(Language language) {
      if (colorLanguage(language)) {
        return Scope::Line;
      }
      return textSyntax(language) == TextSyntax::Code ? Scope::Strings : Scope::None;
    }

    // A match and the index where scanning resumes, which may lie past a suffix that is not part of the code.
    struct Found {
      ColorMatch match;
      std::size_t next = 0;
    };

    struct LineContext {
      std::string_view line;
      Language language = Language::None;
      bool code = false; // CSS notations only inside strings
      char quote = 0;    // the open string's quote in code, or 0
      std::size_t stringStart = 0;
      bool urlRun = false; // inside a run of text that holds "://"
      std::size_t firstNonSpace = 0;
    };

    bool lineCommentAt(std::string_view line, std::size_t i, CommentStyle style) {
      const auto rest = line.substr(i);
      const bool spaced = i == 0 || isBlank(line[i - 1]);
      switch (style) {
      case CommentStyle::Hash:
        return line[i] == '#' && spaced;
      case CommentStyle::Semi:
        return (line[i] == ';' || line[i] == '#') && spaced;
      case CommentStyle::Dash:
        return rest.starts_with("--");
      case CommentStyle::None:
      case CommentStyle::Slash:
        return rest.starts_with("//") || rest.starts_with("/*");
      }
      return false;
    }

    // "url(#..." or "url('#...".
    bool afterUrlOpener(std::string_view line, std::size_t i) {
      auto k = i;
      if (k > 0 && (line[k - 1] == '"' || line[k - 1] == '\'')) {
        --k;
      }
      return k >= 4 && equalsIgnoringCase(line.substr(k - 4, 4), "url(");
    }

    // A quoted value of an href or src attribute: href = "#...".
    bool inLinkAttribute(std::string_view line, std::size_t i) {
      if (i == 0 || (line[i - 1] != '"' && line[i - 1] != '\'')) {
        return false;
      }
      auto k = i - 1;
      while (k > 0 && isBlank(line[k - 1])) {
        --k;
      }
      if (k == 0 || line[k - 1] != '=') {
        return false;
      }
      --k;
      while (k > 0 && isBlank(line[k - 1])) {
        --k;
      }
      const auto name = line.substr(0, k);
      return endsIgnoringCase(name, "href") || endsIgnoringCase(name, "src");
    }

    Rgba hexColor(std::string_view digits) {
      const auto nibble = [&](std::size_t k) { return hexValue(digits[k]); };
      if (digits.size() <= 4) {
        const auto twice = [&](std::size_t k) { return static_cast<std::uint8_t>(nibble(k) * 17U); };
        return {.r = twice(0), .g = twice(1), .b = twice(2), .a = digits.size() == 4 ? twice(3) : std::uint8_t{255}};
      }
      const auto byte = [&](std::size_t k) {
        return static_cast<std::uint8_t>(nibble(2 * k) * 16U + nibble(2 * k + 1));
      };
      return {.r = byte(0), .g = byte(1), .b = byte(2), .a = digits.size() == 8 ? byte(3) : std::uint8_t{255}};
    }

    std::optional<Found> hashColor(const LineContext& c, std::size_t i) {
      const auto line = c.line;
      if (i > 0 && (isWordByte(line[i - 1]) || std::string_view{"#&/\\"}.contains(line[i - 1]))) {
        return std::nullopt;
      }
      if (c.urlRun || afterUrlOpener(line, i) || inLinkAttribute(line, i)) {
        return std::nullopt;
      }
      auto end = i + 1;
      while (end < line.size() && isWordByte(line[end])) {
        ++end;
      }
      const auto digits = line.substr(i + 1, end - i - 1);
      const auto size = digits.size();
      if ((size != 3 && size != 4 && size != 6 && size != 8)
          || !std::ranges::all_of(digits, isAsciiHexDigit)
          || (end < line.size() && line[end] == '-')) {
        return std::nullopt;
      }
      if (cssLike(c.language)) {
        auto k = end;
        while (k < line.size() && isBlank(line[k])) {
          ++k;
        }
        if (k < line.size() && std::string_view{"{.[>+~:"}.contains(line[k])) {
          return std::nullopt;
        }
      } else if (i == c.firstNonSpace) {
        return std::nullopt;
      }
      if (!colorLanguage(c.language) && std::ranges::all_of(digits, isAsciiDigit)) {
        const bool wholeString = c.quote != 0 && i == c.stringStart && end < line.size() && line[end] == c.quote;
        if (!wholeString) {
          return std::nullopt;
        }
      }
      return Found{.match = {.offset = i, .length = end - i, .color = hexColor(digits)}, .next = end};
    }

    std::optional<Found> hexLiteral(std::string_view line, std::size_t i) {
      if (i > 0 && (isWordByte(line[i - 1]) || line[i - 1] == '.')) {
        return std::nullopt;
      }
      auto end = i + 2;
      while (end < line.size() && isAsciiHexDigit(line[end])) {
        ++end;
      }
      const auto digits = line.substr(i + 2, end - i - 2);
      if (digits.size() != 6 && digits.size() != 8) {
        return std::nullopt;
      }
      auto next = end;
      while (next < line.size() && next - end < 3 && std::string_view{"uUlL"}.contains(line[next])) {
        ++next;
      }
      if (next < line.size() && isWordByte(line[next])) {
        return std::nullopt;
      }
      auto color = hexColor(digits);
      if (digits.size() == 8) {
        color = {.r = color.g, .g = color.b, .b = color.a, .a = color.r};
      }
      return Found{.match = {.offset = i, .length = end - i, .color = color}, .next = next};
    }

    std::optional<Found> functionAt(std::string_view line, std::size_t i) {
      if (i > 0 && (isWordByte(line[i - 1]) || std::string_view{"-.$@#"}.contains(line[i - 1]))) {
        return std::nullopt;
      }
      const auto notation = cssColorFunction(line.substr(i));
      if (!notation) {
        return std::nullopt;
      }
      return Found{
          .match = {.offset = i, .length = notation->length, .color = notation->color}, .next = i + notation->length
      };
    }

    std::optional<Found> nameAt(std::string_view line, std::size_t i, std::size_t end) {
      if (i > 0 && (isWordByte(line[i - 1]) || std::string_view{"-.$@#%"}.contains(line[i - 1]))) {
        return std::nullopt;
      }
      if (end < line.size() && std::string_view{"-(:"}.contains(line[end])) {
        return std::nullopt;
      }
      const auto color = cssColorName(line.substr(i, end - i));
      if (!color) {
        return std::nullopt;
      }
      return Found{.match = {.offset = i, .length = end - i, .color = *color}, .next = end};
    }

    std::size_t wordEnd(std::string_view line, std::size_t i) {
      while (i < line.size() && isWordByte(line[i])) {
        ++i;
      }
      return i;
    }

    // Appends the matches of one line, shifted by base, up to limit.
    void scanLine(
        std::string_view line, std::size_t base, const FileKind& kind, bool allowNames, std::size_t limit,
        std::vector<ColorMatch>& out
    ) {
      const auto scope = scopeOf(kind.language);
      if (scope == Scope::None) {
        return;
      }
      LineContext c{.line = line, .language = kind.language, .code = scope == Scope::Strings};
      c.firstNonSpace = skipBlanks(line, 0);
      std::size_t found = 0;
      std::size_t i = 0;
      // Every pass moves i forward.
      while (i < line.size() && found < limit) {
        const char ch = line[i];
        if (c.code && c.quote == 0) {
          if (lineCommentAt(line, i, kind.comment)) {
            break;
          }
          if (ch == '"' || ch == '\'' || ch == '`') {
            c.quote = ch;
            c.stringStart = i + 1;
            c.urlRun = false;
            ++i;
            continue;
          }
        } else if (c.code && (ch == '\\' || ch == c.quote)) {
          c.quote = ch == c.quote ? '\0' : c.quote;
          c.urlRun = c.urlRun && ch == '\\';
          i += ch == '\\' ? 2 : 1;
          continue;
        }
        c.urlRun = !kUrlBreaks.contains(ch) && (c.urlRun || line.substr(i).starts_with("://"));
        const bool inScope = !c.code || c.quote != 0;
        std::optional<Found> hit;
        auto skip = i + 1;
        if (ch == '#' && inScope) {
          hit = hashColor(c, i);
        } else if (isWordByte(ch)) {
          skip = wordEnd(line, i);
          if (c.code && ch == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
            hit = hexLiteral(line, i);
          } else if (inScope && isAsciiAlpha(ch)) {
            hit = functionAt(line, i);
            if (!hit && allowNames) {
              hit = nameAt(line, i, skip);
            }
          }
        }
        if (!hit) {
          i = skip;
          continue;
        }
        hit->match.offset += base;
        out.push_back(hit->match);
        ++found;
        i = hit->next;
      }
    }
  } // namespace

  bool colorLanguage(Language language) {
    switch (language) {
    case Language::Css:
    case Language::Scss:
    case Language::Json:
    case Language::JsonLines:
    case Language::Toml:
    case Language::Yaml:
    case Language::Ini:
    case Language::DesktopEntry:
    case Language::Xml:
    case Language::Html:
      return true;
    default:
      return false;
    }
  }

  std::vector<ColorMatch> findLineColors(std::string_view line, const FileKind& kind, bool allowNames) {
    std::vector<ColorMatch> out;
    scanLine(line, 0, kind, allowNames, kMaxColorsPerLine, out);
    return out;
  }

  std::vector<ColorMatch> findColors(std::string_view text, const FileKind& kind, bool allowNames) {
    std::vector<ColorMatch> out;
    if (scopeOf(kind.language) == Scope::None) {
      return out;
    }
    std::size_t pos = 0;
    // Each pass reads one line and moves pos past its line break, or leaves.
    while (out.size() < kMaxColorsPerFile) {
      const auto lineBreak = text.find_first_of("\r\n", pos);
      const auto end = lineBreak == std::string_view::npos ? text.size() : lineBreak;
      const auto room = std::min(kMaxColorsPerLine, kMaxColorsPerFile - out.size());
      scanLine(text.substr(pos, end - pos), pos, kind, allowNames, room, out);
      if (lineBreak == std::string_view::npos) {
        break;
      }
      const bool crlf = text[lineBreak] == '\r' && lineBreak + 1 < text.size() && text[lineBreak + 1] == '\n';
      pos = lineBreak + (crlf ? 2 : 1);
    }
    return out;
  }
} // namespace luciole::formats
