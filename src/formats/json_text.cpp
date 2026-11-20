#include "formats/json_text.h"

#include "formats/ascii.h"
#include "formats/text_lines.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <utility>

namespace luciole::formats {
  namespace {
    using tokenize::codePoints;
    using tokenize::isNumber;
    using tokenize::kJsonSpace;
    using tokenize::kJsonStop;
    using tokenize::LineWriter;
    using tokenize::skipJsonSpace;
    using tokenize::stringEnd;

    std::string problemAt(std::string_view text, std::size_t pos, std::string_view what) {
      std::size_t line = 1;
      std::size_t lineStart = 0;
      for (std::size_t i = 0; i < pos; ++i) {
        if (text[i] == '\n' || (text[i] == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'))) {
          ++line;
          lineStart = i + 1;
        }
      }
      const auto column = codePoints(text.substr(lineStart, pos - lineStart), std::string_view::npos) + 1;
      return std::format("{} at line {}, column {}", what, line, column);
    }

    std::string unexpectedAt(std::string_view text, std::size_t pos) {
      const auto byte = static_cast<unsigned char>(text[pos]);
      if (byte > 0x20 && byte < 0x7F) {
        return problemAt(text, pos, std::format("unexpected character '{}'", text[pos]));
      }
      return problemAt(text, pos, std::format("unexpected byte 0x{:02X}", byte));
    }

    using JsonEnd = std::expected<std::size_t, std::string>;

    // With `partial`, a token that runs to the end of the text and could still be completed ends there.
    JsonEnd jsonStringEnd(std::string_view text, std::size_t start, bool partial) {
      for (std::size_t j = start + 1; j < text.size(); ++j) {
        const auto byte = static_cast<unsigned char>(text[j]);
        if (byte == '"') {
          return j + 1;
        }
        if (byte < 0x20) {
          return std::unexpected(problemAt(text, j, "control character in a string"));
        }
        if (byte != '\\') {
          continue;
        }
        if (j + 1 >= text.size()) {
          break;
        }
        const char escape = text[j + 1];
        const auto digits = text.substr(j + 2, 4);
        const bool hex = escape == 'u' && std::ranges::all_of(digits, isAsciiHexDigit);
        if (hex && digits.size() == 4) {
          j += 5;
        } else if (hex && partial) {
          return text.size();
        } else if (escape != 'u' && std::string_view{"\"\\/bfnrt"}.contains(escape)) {
          ++j;
        } else {
          return std::unexpected(problemAt(text, j, "invalid escape in a string"));
        }
      }
      if (partial) {
        return text.size();
      }
      return std::unexpected(problemAt(text, start, "unterminated string"));
    }

    JsonEnd jsonNumberEnd(std::string_view text, std::size_t start, bool partial) {
      std::size_t j = start;
      const auto digits = [&] {
        const auto from = j;
        while (j < text.size() && isAsciiDigit(text[j])) {
          ++j;
        }
        return j > from;
      };
      const auto at = [&](std::string_view chars) { return j < text.size() && chars.contains(text[j]); };
      const auto invalid = [&] {
        if (partial && j >= text.size()) {
          return JsonEnd{text.size()};
        }
        return JsonEnd{std::unexpect, problemAt(text, start, "invalid number")};
      };
      j += at("-") ? 1 : 0;
      if (at("0")) {
        ++j;
      } else if (!digits()) {
        return invalid();
      }
      if (at(".")) {
        ++j;
        if (!digits()) {
          return invalid();
        }
      }
      if (at("eE")) {
        ++j;
        j += at("+-") ? 1 : 0;
        if (!digits()) {
          return invalid();
        }
      }
      return j;
    }

    JsonEnd jsonScalarEnd(std::string_view text, std::size_t i, bool partial) {
      const char c = text[i];
      if (c == '"') {
        return jsonStringEnd(text, i, partial);
      }
      if (c == '-' || isAsciiDigit(c)) {
        return jsonNumberEnd(text, i, partial);
      }
      for (const std::string_view literal : {"true", "false", "null"}) {
        if (text.substr(i).starts_with(literal)) {
          return i + literal.size();
        }
        if (partial && literal.starts_with(text.substr(i))) {
          return text.size();
        }
      }
      return std::unexpected(unexpectedAt(text, i));
    }

    // True when the string at `start` ends with an unescaped closing quote at `end - 1`.
    bool closedString(std::string_view text, std::size_t start, std::size_t end) {
      if (end < start + 2 || text[end - 1] != '"') {
        return false;
      }
      std::size_t backslashes = 0;
      // Moves back from the quote, at most to the character after the opening quote.
      for (auto k = end - 1; k > start + 1 && text[k - 1] == '\\'; --k) {
        ++backslashes;
      }
      return backslashes % 2 == 0;
    }

    enum class JsonExpect : std::uint8_t { Value, FirstValue, Key, FirstKey, Colon, Next };
  } // namespace

  std::optional<std::string> jsonProblem(std::string_view text, JsonExtent extent) {
    const bool partial = extent == JsonExtent::Start;
    std::vector<bool> open; // containers still open, innermost last; true for an object
    std::size_t deepest = 0;
    auto expect = JsonExpect::Value;
    bool done = false;
    std::size_t i = 0;
    const auto valueEnded = [&] {
      done = open.empty();
      expect = JsonExpect::Next;
    };
    // Every pass consumes at least one byte or leaves, so text.size() + 1 passes suffice.
    for (std::size_t pass = 0; pass <= text.size(); ++pass) {
      i = skipJsonSpace(text, i);
      if (i >= text.size()) {
        break;
      }
      if (done) {
        return problemAt(text, i, "unexpected text after the value");
      }
      const char c = text[i];
      if ((expect == JsonExpect::FirstValue && c == ']')
          || (expect == JsonExpect::FirstKey && c == '}')
          || (expect == JsonExpect::Next && c == (open.back() ? '}' : ']'))) {
        open.pop_back();
        ++i;
        valueEnded();
      } else if (expect == JsonExpect::Next) {
        if (c != ',') {
          return unexpectedAt(text, i);
        }
        expect = open.back() ? JsonExpect::Key : JsonExpect::Value;
        ++i;
      } else if (expect == JsonExpect::Colon) {
        if (c != ':') {
          return unexpectedAt(text, i);
        }
        expect = JsonExpect::Value;
        ++i;
      } else if (expect == JsonExpect::Key || expect == JsonExpect::FirstKey) {
        if (c != '"') {
          return unexpectedAt(text, i);
        }
        const auto end = jsonStringEnd(text, i, partial);
        if (!end) {
          return end.error();
        }
        i = *end;
        expect = JsonExpect::Colon;
      } else if (c == '{' || c == '[') {
        open.push_back(c == '{');
        deepest = std::max(deepest, open.size());
        expect = c == '{' ? JsonExpect::FirstKey : JsonExpect::FirstValue;
        ++i;
      } else {
        const auto end = jsonScalarEnd(text, i, partial);
        if (!end) {
          return end.error();
        }
        i = *end;
        valueEnded();
      }
    }
    if (!done && !partial) {
      return std::string{"unexpected end of input"};
    }
    if (deepest > kMaxJsonDepth) {
      return std::format("nested deeper than {} levels", kMaxJsonDepth);
    }
    return std::nullopt;
  }

  PrintedJson printJson(std::string_view text) {
    PrintedJson out;
    TextLine current;
    const LineWriter w{.s = text, .base = 0, .out = &current};
    std::size_t depth = 0;
    const auto flush = [&] {
      if (!current.empty()) {
        if (depth > 0) {
          const auto width = static_cast<std::uint32_t>(2 * std::min(depth, kMaxJsonDepth));
          current.insert(
              current.begin(), {.offset = current.front().offset, .length = width, .tokenClass = TokenClass::Space}
          );
        }
        out.lines.push_back(std::move(current));
      }
      current.clear();
    };
    std::size_t i = skipJsonSpace(text, 0);
    // Every pass moves i forward or leaves the loop.
    while (i < text.size()) {
      if (out.lines.size() >= kMaxTextLines) {
        out.capped = true;
        break;
      }
      const char c = text[i];
      if (c == '{' || c == '[') {
        const auto j = skipJsonSpace(text, i + 1);
        w.add(i, i + 1, TokenClass::Punct);
        if (j < text.size() && text[j] == (c == '{' ? '}' : ']')) {
          w.add(j, j + 1, TokenClass::Punct);
          i = j + 1;
        } else {
          flush();
          ++depth;
          ++i;
        }
      } else if (c == '}' || c == ']') {
        flush();
        depth -= depth > 0 ? 1 : 0;
        w.add(i, i + 1, TokenClass::Punct);
        ++i;
      } else if (c == ',') {
        w.add(i, i + 1, TokenClass::Punct);
        flush();
        ++i;
      } else if (c == ':') {
        w.add(i, i + 1, TokenClass::Punct);
        current.push_back({.offset = static_cast<std::uint32_t>(i + 1), .length = 1, .tokenClass = TokenClass::Space});
        ++i;
      } else if (c == '"') {
        const auto e = stringEnd(text, i);
        const auto next = skipJsonSpace(text, e);
        w.add(i, e, next < text.size() && text[next] == ':' ? TokenClass::Key : TokenClass::String);
        out.endsEarly = e == text.size() && !closedString(text, i, e);
        i = e;
      } else if (kJsonSpace.contains(c)) {
        ++i;
      } else {
        auto e = i;
        while (e < text.size() && !kJsonStop.contains(text[e])) {
          ++e;
        }
        w.add(i, e, isNumber(text.substr(i, e - i)) ? TokenClass::Number : TokenClass::Literal);
        i = e;
      }
    }
    const bool inside = depth > 0 || out.endsEarly;
    flush();
    if (out.lines.size() > kMaxTextLines) {
      out.lines.resize(kMaxTextLines);
      out.capped = true;
    }
    out.endsEarly = inside && !out.capped;
    return out;
  }
} // namespace luciole::formats
