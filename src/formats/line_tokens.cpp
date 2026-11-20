#include "formats/line_tokens.h"

#include "formats/ascii.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace luciole::formats::tokenize {
  namespace {
    constexpr std::string_view kNameEnd = " \t\r\n/>=<";
    constexpr std::array<std::string_view, 15> kDiffHeaders{
        "diff ",
        "index ",
        "--- ",
        "+++ ",
        "new file mode",
        "deleted file mode",
        "old mode",
        "new mode",
        "similarity index",
        "dissimilarity index",
        "rename from",
        "rename to",
        "copy from",
        "copy to",
        "Binary files"
    };

    std::size_t yamlSeparator(std::string_view rest) {
      std::size_t j = 0;
      if (rest.starts_with('"') || rest.starts_with('\'')) {
        j = stringEnd(rest, 0);
      } else if (rest.starts_with('[') || rest.starts_with('{') || rest.starts_with('#')) {
        return std::string_view::npos;
      }
      for (; j < rest.size(); ++j) {
        if (rest[j] == ':' && (j + 1 == rest.size() || rest[j + 1] == ' ')) {
          return j;
        }
        if (rest[j] == '#' && j > 0 && rest[j - 1] == ' ') {
          return std::string_view::npos;
        }
      }
      return std::string_view::npos;
    }

    void yamlLine(const LineWriter& w, ScanState& state) {
      const auto indent = skipBlanks(w.s, 0);
      w.add(0, indent, TokenClass::Plain);
      auto rest = w.tail(indent);
      if (rest.s.starts_with('#')) {
        rest.add(0, rest.s.size(), TokenClass::Comment);
        return;
      }
      if (rest.s == "---" || rest.s == "...") {
        rest.add(0, rest.s.size(), TokenClass::Punct);
        return;
      }
      // Each pass consumes one list marker and the spaces after it.
      while (rest.s == "-" || rest.s.starts_with("- ")) {
        const auto gap = skipBlanks(rest.s.substr(1), 0);
        rest.add(0, 1, TokenClass::Punct);
        rest.add(1, 1 + gap, TokenClass::Plain);
        rest = rest.tail(1 + gap);
      }
      const CodeOptions options{.style = CommentStyle::Hash, .family = Family::Yaml};
      const auto sep = yamlSeparator(rest.s);
      if (sep != std::string_view::npos) {
        keyValue(rest, sep, options, state);
      } else {
        scanCode(rest, state, options);
      }
    }

    std::size_t tomlSeparator(std::string_view rest) {
      std::size_t j = rest.starts_with('"') || rest.starts_with('\'') ? stringEnd(rest, 0) : 0;
      for (; j < rest.size(); ++j) {
        if (rest[j] == '=') {
          return j;
        }
        if (rest[j] == '#') {
          return std::string_view::npos;
        }
      }
      return std::string_view::npos;
    }

    void tomlLine(const LineWriter& w, ScanState& state) {
      const CodeOptions options{.style = CommentStyle::Hash, .family = Family::Toml, .triple = true};
      if (state.block) {
        scanCode(w, state, options);
        return;
      }
      const auto indent = skipBlanks(w.s, 0);
      w.add(0, indent, TokenClass::Plain);
      const auto rest = w.tail(indent);
      if (rest.s.starts_with('[')) {
        const bool array = rest.s.starts_with("[[");
        const auto found = rest.s.find(array ? "]]" : "]");
        const auto close = found == std::string_view::npos ? 0 : found + (array ? 2 : 1);
        const auto stop = close > 1 ? close : rest.s.size();
        rest.add(0, stop, TokenClass::Tag);
        scanCode(rest.tail(stop), state, options);
        return;
      }
      const auto sep = tomlSeparator(rest.s);
      if (sep != std::string_view::npos) {
        keyValue(rest, sep, options, state);
      } else {
        scanCode(rest, state, options);
      }
    }

    void iniLine(const LineWriter& w, ScanState& state) {
      const auto indent = skipBlanks(w.s, 0);
      w.add(0, indent, TokenClass::Plain);
      const auto rest = w.tail(indent);
      if (rest.s.starts_with(';') || rest.s.starts_with('#')) {
        rest.add(0, rest.s.size(), TokenClass::Comment);
        return;
      }
      if (rest.s.starts_with('[')) {
        rest.add(0, rest.s.size(), TokenClass::Tag);
        return;
      }
      const auto sep = std::min(rest.s.find('='), rest.s.find(':'));
      if (sep != std::string_view::npos && sep > 0) {
        keyValue(rest, sep, {.style = CommentStyle::Semi, .family = Family::Ini}, state);
      } else {
        rest.add(0, rest.s.size(), TokenClass::Plain);
      }
    }

    // Attributes of an open tag from i; returns the index after ">" or "/>" (the tag closes), or the line end.
    std::size_t xmlAttributes(const LineWriter& w, std::size_t i, ScanState& state) {
      const auto s = w.s;
      // Every pass moves i forward or returns.
      while (i < s.size()) {
        const char c = s[i];
        if (isBlank(c)) {
          w.add(i, i + 1, TokenClass::Plain);
          ++i;
        } else if (c == '>' || s.substr(i).starts_with("/>")) {
          const std::size_t n = c == '>' ? 1 : 2;
          w.add(i, i + n, TokenClass::Punct);
          state.inTag = false;
          return i + n;
        } else if (c == '=') {
          w.add(i, i + 1, TokenClass::Punct);
          ++i;
        } else if (c == '"' || c == '\'') {
          const auto e = stringEnd(s, i);
          w.add(i, e, TokenClass::String);
          i = e;
        } else {
          auto e = i;
          while (e < s.size() && !kNameEnd.contains(s[e])) {
            ++e;
          }
          e = std::max(e, i + 1);
          w.add(i, e, TokenClass::Attr);
          i = e;
        }
      }
      return i;
    }

    void xmlLine(const LineWriter& w, ScanState& state) {
      struct Mark {
        std::string_view open;
        std::string_view close;
        TokenClass tokenClass;
      };
      constexpr std::array<Mark, 3> kMarks{{
          {.open = "<!--", .close = "-->", .tokenClass = TokenClass::Comment},
          {.open = "<?", .close = "?>", .tokenClass = TokenClass::Comment},
          {.open = "<![CDATA[", .close = "]]>", .tokenClass = TokenClass::Plain},
      }};
      const auto s = w.s;
      std::size_t i = 0;
      // Every pass moves i forward or leaves the loop.
      while (i < s.size()) {
        if (state.block) {
          i = inBlock(w, i, state);
          continue;
        }
        if (state.inTag) {
          i = xmlAttributes(w, i, state);
          continue;
        }
        const auto open = s.find('<', i);
        if (open == std::string_view::npos) {
          w.add(i, s.size(), TokenClass::Plain);
          break;
        }
        w.add(i, open, TokenClass::Plain);
        const auto* const mark =
            std::ranges::find_if(kMarks, [&](const Mark& m) { return s.substr(open).starts_with(m.open); });
        if (mark != kMarks.end()) {
          state.block = Block{.end = mark->close, .tokenClass = mark->tokenClass};
          w.add(open, open + mark->open.size(), mark->tokenClass);
          i = open + mark->open.size();
          continue;
        }
        const std::size_t slash = open + 1 < s.size() && s[open + 1] == '/' ? 2 : 1;
        w.add(open, open + slash, TokenClass::Punct);
        auto e = open + slash;
        while (e < s.size() && !kNameEnd.contains(s[e])) {
          ++e;
        }
        w.add(open + slash, e, TokenClass::Tag);
        state.inTag = true;
        i = e;
      }
    }

    // One JSON value per line, colored as the printer colors it, with the line's own spacing.
    void jsonLine(const LineWriter& w) {
      const auto s = w.s;
      std::size_t i = 0;
      // Every pass moves i forward.
      while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
          const auto e = stringEnd(s, i);
          const auto next = skipJsonSpace(s, e);
          w.add(i, e, next < s.size() && s[next] == ':' ? TokenClass::Key : TokenClass::String);
          i = e;
        } else if (std::string_view{"{}[],:"}.contains(c)) {
          w.add(i, i + 1, TokenClass::Punct);
          ++i;
        } else if (kJsonSpace.contains(c)) {
          w.add(i, i + 1, TokenClass::Plain);
          ++i;
        } else {
          auto e = i;
          while (e < s.size() && !kJsonStop.contains(s[e])) {
            ++e;
          }
          w.add(i, e, isNumber(s.substr(i, e - i)) ? TokenClass::Number : TokenClass::Literal);
          i = e;
        }
      }
    }

    // A count of 1 to 9 digits at i; i moves past it.
    std::optional<std::size_t> diffCount(std::string_view s, std::size_t& i) {
      const auto from = i;
      std::size_t value = 0;
      while (i < s.size() && isAsciiDigit(s[i]) && i - from < 9) {
        value = value * 10 + static_cast<std::size_t>(s[i] - '0');
        ++i;
      }
      if (i == from || (i < s.size() && isAsciiDigit(s[i]))) {
        return std::nullopt;
      }
      return value;
    }

    // "start[,count]" at i; the count defaults to 1.
    std::optional<std::size_t> diffRange(std::string_view s, std::size_t& i) {
      if (!diffCount(s, i)) {
        return std::nullopt;
      }
      if (i < s.size() && s[i] == ',') {
        ++i;
        return diffCount(s, i);
      }
      return 1;
    }

    // Old and new line counts of "@@ -a[,b] +c[,d] @@"; empty for any other shape.
    std::optional<std::pair<std::size_t, std::size_t>> hunkCounts(std::string_view s) {
      if (!s.starts_with("@@ -")) {
        return std::nullopt;
      }
      std::size_t i = 4;
      const auto oldCount = diffRange(s, i);
      if (!oldCount || !s.substr(i).starts_with(" +")) {
        return std::nullopt;
      }
      i += 2;
      const auto newCount = diffRange(s, i);
      if (!newCount || !s.substr(i).starts_with(" @@")) {
        return std::nullopt;
      }
      return std::pair{*oldCount, *newCount};
    }

    // Inside a hunk the counts from its header decide, so "--- x" there is a removed line, not a file header.
    void diffLine(const LineWriter& w, ScanState& state) {
      const auto s = w.s;
      if (state.oldLeft > 0 || state.newLeft > 0) {
        const char c = s.empty() ? ' ' : s[0];
        if (c == ' ') {
          state.oldLeft -= state.oldLeft > 0 ? 1 : 0;
          state.newLeft -= state.newLeft > 0 ? 1 : 0;
          w.add(0, s.size(), TokenClass::Plain);
          return;
        }
        if (c == '-' && state.oldLeft > 0) {
          --state.oldLeft;
          w.add(0, s.size(), TokenClass::Removed);
          return;
        }
        if (c == '+' && state.newLeft > 0) {
          --state.newLeft;
          w.add(0, s.size(), TokenClass::Added);
          return;
        }
        if (c == '\\') {
          w.add(0, s.size(), TokenClass::Comment);
          return;
        }
        state.oldLeft = 0;
        state.newLeft = 0;
      }
      if (s.starts_with("@@")) {
        const auto counts = hunkCounts(s);
        state.oldLeft = counts ? counts->first : 0;
        state.newLeft = counts ? counts->second : 0;
        w.add(0, s.size(), TokenClass::Hunk);
      } else if (std::ranges::any_of(kDiffHeaders, [&](std::string_view prefix) { return s.starts_with(prefix); })) {
        w.add(0, s.size(), TokenClass::Header);
      } else if (s.starts_with('+')) {
        w.add(0, s.size(), TokenClass::Added);
      } else if (s.starts_with('-')) {
        w.add(0, s.size(), TokenClass::Removed);
      } else if (s.starts_with('\\')) {
        w.add(0, s.size(), TokenClass::Comment);
      } else {
        w.add(0, s.size(), TokenClass::Plain);
      }
    }
  } // namespace

  void tokenizeLine(const LineWriter& w, TextSyntax syntax, const CodeOptions& code, ScanState& state) {
    switch (syntax) {
    case TextSyntax::Yaml:
      yamlLine(w, state);
      break;
    case TextSyntax::Toml:
      tomlLine(w, state);
      break;
    case TextSyntax::Ini:
      iniLine(w, state);
      break;
    case TextSyntax::Xml:
      xmlLine(w, state);
      break;
    case TextSyntax::Code:
      scanCode(w, state, code);
      break;
    case TextSyntax::JsonLines:
      jsonLine(w);
      break;
    case TextSyntax::Diff:
      diffLine(w, state);
      break;
    default:
      w.add(0, w.s.size(), TokenClass::Plain);
      break;
    }
  }
} // namespace luciole::formats::tokenize
