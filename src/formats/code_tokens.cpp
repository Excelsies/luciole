#include "formats/code_tokens.h"

#include "formats/ascii.h"
#include "formats/unicode_space.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace luciole::formats::tokenize {
  namespace {
    constexpr std::string_view kSingles = ",[]{}()=;";
    constexpr std::size_t kMaxKeywordBytes = 32;
    constexpr std::array<std::string_view, 11> kYamlLiterals{"true", "false", "null", "yes",   "no",  "on",
                                                             "off",  "~",     "True", "False", "None"};
    constexpr std::array<std::string_view, 4> kTomlLiterals{"true", "false", "inf", "nan"};
    constexpr std::array<std::string_view, 6> kIniLiterals{"true", "false", "yes", "no", "on", "off"};
    constexpr std::array<std::string_view, 9> kCodeLiterals{"true", "false", "null", "nil",      "None",
                                                            "True", "False", "NULL", "undefined"};

    std::span<const std::string_view> literals(Family family) {
      switch (family) {
      case Family::Yaml:
        return kYamlLiterals;
      case Family::Toml:
        return kTomlLiterals;
      case Family::Ini:
        return kIniLiterals;
      case Family::Code:
        return kCodeLiterals;
      }
      return {};
    }

    bool commentAt(std::string_view s, std::size_t i, CommentStyle style) {
      const bool spaced = i == 0 || isBlank(s[i - 1]);
      switch (style) {
      case CommentStyle::Slash:
        return s.substr(i).starts_with("//");
      case CommentStyle::Dash:
        return s.substr(i).starts_with("--");
      case CommentStyle::Hash:
        return s[i] == '#' && spaced;
      case CommentStyle::Semi:
        return (s[i] == ';' || s[i] == '#') && spaced;
      case CommentStyle::None:
        return false;
      }
      return false;
    }

    std::size_t wordEnd(std::string_view s, std::size_t i, CommentStyle style) {
      std::size_t j = i;
      while (j < s.size()) {
        const char c = s[j];
        const char next = j + 1 < s.size() ? s[j + 1] : '\0';
        if (isBlank(c) || isQuote(c) || kSingles.contains(c)) {
          break;
        }
        if (j > i && style == CommentStyle::Slash && c == '/' && (next == '/' || next == '*')) {
          break;
        }
        if (j > i && style == CommentStyle::Dash && c == '-' && next == '-') {
          break;
        }
        ++j;
      }
      return std::max(j, i + 1);
    }

    // dddd-dd-dd, optionally followed by T or a space and a run of digits, ':' and '.', and an optional Z.
    bool isDate(std::string_view w) {
      constexpr std::string_view kShape = "dddd-dd-dd";
      if (w.size() < kShape.size()) {
        return false;
      }
      for (std::size_t i = 0; i < kShape.size(); ++i) {
        if (kShape[i] == 'd' ? !isAsciiDigit(w[i]) : w[i] != '-') {
          return false;
        }
      }
      if (w.size() == kShape.size()) {
        return true;
      }
      if (w[kShape.size()] != 'T' && w[kShape.size()] != ' ') {
        return false;
      }
      auto i = kShape.size() + 1;
      while (i < w.size() && (isAsciiDigit(w[i]) || w[i] == ':' || w[i] == '.')) {
        ++i;
      }
      if (i == kShape.size() + 1) {
        return false;
      }
      return i == w.size() || (i + 1 == w.size() && w[i] == 'Z');
    }

    bool isKeyword(std::string_view word, const KeywordSet& set) {
      if (set.words.empty() || word.size() > kMaxKeywordBytes) {
        return false;
      }
      if (!set.caseless) {
        return std::ranges::binary_search(set.words, word);
      }
      std::array<char, kMaxKeywordBytes> lower{};
      std::ranges::transform(word, lower.begin(), toAsciiLower);
      return std::ranges::binary_search(set.words, std::string_view{lower.data(), word.size()});
    }

    // Length of the identifier a word starts with, allowing one leading '#', '@' or '!'; 0 when there is none.
    std::size_t identifierLength(std::string_view word) {
      std::size_t k = word.size() > 1 && std::string_view{"#@!"}.contains(word[0]) ? 1 : 0;
      if (k >= word.size() || (!isAsciiAlpha(word[k]) && word[k] != '_')) {
        return 0;
      }
      while (k < word.size() && (isAsciiAlpha(word[k]) || isAsciiDigit(word[k]) || word[k] == '_')) {
        ++k;
      }
      return k;
    }

    void addWord(const LineWriter& w, std::size_t begin, std::size_t end, const CodeOptions& options) {
      const auto word = w.s.substr(begin, end - begin);
      if (isNumber(word) || isDate(word)) {
        w.add(begin, end, TokenClass::Number);
        return;
      }
      if (std::ranges::contains(literals(options.family), word)) {
        w.add(begin, end, TokenClass::Literal);
        return;
      }
      const auto identifier = identifierLength(word);
      if (identifier > 0 && isKeyword(word.substr(0, identifier), options.keywords)) {
        w.add(begin, begin + identifier, TokenClass::Keyword);
        w.add(begin + identifier, end, TokenClass::Plain);
        return;
      }
      w.add(begin, end, TokenClass::Plain);
    }

  } // namespace

  void scanCode(const LineWriter& w, ScanState& state, const CodeOptions& options) {
    const auto s = w.s;
    std::size_t i = 0;
    // Every pass moves i forward or leaves the loop.
    while (i < s.size()) {
      if (state.block) {
        i = inBlock(w, i, state);
        continue;
      }
      const char c = s[i];
      const auto rest = s.substr(i);
      if (options.blocks && rest.starts_with("/*")) {
        state.block = Block{.end = "*/", .tokenClass = TokenClass::Comment};
        w.add(i, i + 2, TokenClass::Comment);
        i += 2;
      } else if (commentAt(s, i, options.style)) {
        w.add(i, s.size(), TokenClass::Comment);
        break;
      } else if (options.triple && (rest.starts_with(R"(""")") || rest.starts_with("'''"))) {
        state.block = Block{.end = c == '"' ? R"(""")" : "'''", .tokenClass = TokenClass::String};
        w.add(i, i + 3, TokenClass::String);
        i += 3;
      } else if (isQuote(c)) {
        const auto e = stringEnd(s, i);
        w.add(i, e, TokenClass::String);
        i = e;
      } else if (isBlank(c) || kSingles.contains(c)) {
        w.add(i, i + 1, TokenClass::Plain);
        ++i;
      } else {
        const auto e = wordEnd(s, i, options.style);
        addWord(w, i, e, options);
        i = e;
      }
    }
  }

  void keyValue(const LineWriter& w, std::size_t sep, const CodeOptions& options, ScanState& state) {
    if (sep >= w.s.size()) {
      w.add(0, w.s.size(), TokenClass::Key);
      return;
    }
    const auto keyEnd = sep - trailingUnicodeSpace(w.s.substr(0, sep));
    w.add(0, keyEnd, TokenClass::Key);
    w.add(keyEnd, sep, TokenClass::Plain);
    w.add(sep, sep + 1, TokenClass::Punct);
    scanCode(w.tail(sep + 1), state, options);
  }
} // namespace luciole::formats::tokenize
