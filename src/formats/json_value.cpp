#include "formats/json_value.h"

#include "formats/ascii.h"
#include "formats/utf8.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>
#include <optional>

namespace luciole::formats {
  namespace {
    constexpr std::uint32_t kReplacement = 0xFFFD;

    class Parser {
    public:
      Parser(std::string_view text, const JsonLimits& limits) : m_text(text), m_limits(limits) {}

      std::expected<JsonValue, std::string> run() {
        JsonValue root;
        if (m_text.size() > m_limits.maxBytes) {
          return std::unexpected("the JSON text is larger than the limit");
        }
        bool ok = parseValue(root, 0);
        if (ok) {
          skipSpace();
          ok = m_pos == m_text.size() || fail("unexpected text after the JSON value");
        }
        if (!ok) {
          return std::unexpected(std::format("{} at byte {}", m_error, m_pos));
        }
        return root;
      }

    private:
      bool fail(std::string_view message) {
        m_error = message;
        return false;
      }

      bool atEnd() const { return m_pos >= m_text.size(); }

      void skipSpace() {
        while (!atEnd()
               && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r')) {
          ++m_pos;
        }
      }

      bool consume(char c) {
        if (!atEnd() && m_text[m_pos] == c) {
          ++m_pos;
          return true;
        }
        return false;
      }

      bool consumeDigits() {
        const std::size_t start = m_pos;
        while (!atEnd() && isAsciiDigit(m_text[m_pos])) {
          ++m_pos;
        }
        return m_pos > start;
      }

      bool parseValue(JsonValue& out, std::size_t depth) {
        if (++m_values > m_limits.maxValues) {
          return fail("the JSON text holds too many values");
        }
        skipSpace();
        if (atEnd()) {
          return fail("the JSON text ends early");
        }
        switch (m_text[m_pos]) {
        case '{':
          return parseObject(out, depth + 1);
        case '[':
          return parseArray(out, depth + 1);
        case '"':
          out.type = JsonType::String;
          return parseString(out.text);
        case 't':
          out.type = JsonType::Boolean;
          out.boolean = true;
          return parseLiteral("true");
        case 'f':
          out.type = JsonType::Boolean;
          return parseLiteral("false");
        case 'n':
          return parseLiteral("null");
        default:
          out.type = JsonType::Number;
          return parseNumber(out.number);
        }
      }

      bool parseLiteral(std::string_view word) {
        if (!m_text.substr(m_pos).starts_with(word)) {
          return fail("not a JSON value");
        }
        m_pos += word.size();
        return true;
      }

      bool parseNumber(double& out) {
        const std::size_t start = m_pos;
        consume('-');
        if (!consume('0') && !consumeDigits()) {
          return fail("not a JSON value");
        }
        if (consume('.') && !consumeDigits()) {
          return fail("a JSON number has no digits after its point");
        }
        bool negativeExponent = false;
        if (consume('e') || consume('E')) {
          negativeExponent = consume('-');
          if (!negativeExponent) {
            consume('+');
          }
          if (!consumeDigits()) {
            return fail("a JSON number has no exponent digits");
          }
        }
        const auto [ptr, ec] = std::from_chars(m_text.data() + start, m_text.data() + m_pos, out);
        if (ec == std::errc::result_out_of_range && negativeExponent) {
          out = 0.0;
        } else if (ec != std::errc{} || ptr != m_text.data() + m_pos) {
          return fail("a JSON number is out of range");
        }
        return true;
      }

      std::optional<std::uint32_t> hexUnit() {
        if (m_text.size() - m_pos < 4) {
          return std::nullopt;
        }
        std::uint32_t unit = 0;
        const auto [ptr, ec] = std::from_chars(m_text.data() + m_pos, m_text.data() + m_pos + 4, unit, 16);
        if (ec != std::errc{} || ptr != m_text.data() + m_pos + 4) {
          return std::nullopt;
        }
        m_pos += 4;
        return unit;
      }

      // The four hex digits after "\u", and a low surrogate escape that follows a high one.
      bool parseUnicodeEscape(std::string& out) {
        const std::optional<std::uint32_t> unit = hexUnit();
        if (!unit) {
          return fail("a JSON \\u escape needs four hex digits");
        }
        std::uint32_t code = *unit;
        if (code >= 0xDC00 && code <= 0xDFFF) {
          code = kReplacement;
        } else if (code >= 0xD800 && code <= 0xDBFF) {
          const std::size_t afterHigh = m_pos;
          std::optional<std::uint32_t> low;
          if (m_text.substr(m_pos).starts_with("\\u")) {
            m_pos += 2;
            low = hexUnit();
          }
          if (low && *low >= 0xDC00 && *low <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (*low - 0xDC00);
          } else {
            m_pos = afterHigh;
            code = kReplacement;
          }
        }
        appendUtf8(out, code);
        return true;
      }

      bool parseString(std::string& out) {
        ++m_pos;
        // Each pass consumes at least one byte.
        while (!atEnd()) {
          const std::size_t runStart = m_pos;
          while (!atEnd()
                 && m_text[m_pos] != '"'
                 && m_text[m_pos] != '\\'
                 && static_cast<unsigned char>(m_text[m_pos]) >= 0x20) {
            ++m_pos;
          }
          out.append(m_text.substr(runStart, m_pos - runStart));
          if (atEnd()) {
            break;
          }
          const char c = m_text[m_pos++];
          if (c == '"') {
            return true;
          }
          if (c != '\\') {
            return fail("a JSON string holds a control character");
          }
          if (atEnd()) {
            break;
          }
          const char escape = m_text[m_pos++];
          switch (escape) {
          case '"':
          case '\\':
          case '/':
            out.push_back(escape);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u':
            if (!parseUnicodeEscape(out)) {
              return false;
            }
            break;
          default:
            return fail("a JSON string has an unknown escape");
          }
        }
        return fail("a JSON string is not closed");
      }

      bool parseArray(JsonValue& out, std::size_t depth) {
        if (depth > m_limits.maxDepth) {
          return fail("the JSON text is nested too deeply");
        }
        ++m_pos;
        out.type = JsonType::Array;
        skipSpace();
        if (consume(']')) {
          return true;
        }
        // Each pass parses one element, so maxValues bounds the passes.
        while (true) {
          out.items.emplace_back();
          if (!parseValue(out.items.back(), depth)) {
            return false;
          }
          skipSpace();
          if (consume(']')) {
            return true;
          }
          if (!consume(',')) {
            return fail("a JSON array needs ',' or ']'");
          }
        }
      }

      bool parseObject(JsonValue& out, std::size_t depth) {
        if (depth > m_limits.maxDepth) {
          return fail("the JSON text is nested too deeply");
        }
        ++m_pos;
        out.type = JsonType::Object;
        skipSpace();
        if (consume('}')) {
          return true;
        }
        // Each pass parses one member, so maxValues bounds the passes.
        while (true) {
          skipSpace();
          if (atEnd() || m_text[m_pos] != '"') {
            return fail("a JSON object needs a member name");
          }
          std::string key;
          if (!parseString(key)) {
            return false;
          }
          skipSpace();
          if (!consume(':')) {
            return fail("a JSON member name needs ':'");
          }
          out.keys.push_back(std::move(key));
          out.items.emplace_back();
          if (!parseValue(out.items.back(), depth)) {
            return false;
          }
          skipSpace();
          if (consume('}')) {
            orderKeys(out);
            return true;
          }
          if (!consume(',')) {
            return fail("a JSON object needs ',' or '}'");
          }
        }
      }

      // A stable sort keeps repeated names in file order, so find returns the first.
      static void orderKeys(JsonValue& object) {
        object.keyOrder.resize(object.keys.size());
        std::ranges::iota(object.keyOrder, std::size_t{0});
        std::ranges::stable_sort(object.keyOrder, {}, [&](std::size_t position) {
          return std::string_view(object.keys[position]);
        });
      }

      std::string_view m_text;
      JsonLimits m_limits;
      std::size_t m_pos = 0;
      std::size_t m_values = 0;
      std::string m_error;
    };
  } // namespace

  const JsonValue* JsonValue::find(std::string_view key) const {
    if (type != JsonType::Object || keys.size() != items.size() || keyOrder.size() != keys.size()) {
      return nullptr;
    }
    const auto name = [&](std::size_t position) { return std::string_view(keys.at(position)); };
    const auto first = std::ranges::lower_bound(keyOrder, key, {}, name);
    return first != keyOrder.end() && name(*first) == key ? &items.at(*first) : nullptr;
  }

  const JsonValue* JsonValue::item(std::size_t index) const {
    return type == JsonType::Array && index < items.size() ? &items[index] : nullptr;
  }

  std::optional<std::size_t> wholeNumber(const JsonValue* value) {
    constexpr double kExactLimit = 9007199254740992.0; // 2^53
    if (value == nullptr
        || value->type != JsonType::Number
        || !(value->number >= 0.0)
        || !(value->number < kExactLimit)
        || value->number != std::floor(value->number)) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(value->number);
  }

  std::span<const JsonValue> arrayItems(const JsonValue* value) {
    return value == nullptr || value->type != JsonType::Array ? std::span<const JsonValue>{} : value->items;
  }

  bool holdsAnyString(const JsonValue* array, std::initializer_list<std::string_view> texts) {
    return std::ranges::any_of(arrayItems(array), [&](const JsonValue& item) {
      return item.type == JsonType::String && std::ranges::contains(texts, std::string_view(item.text));
    });
  }

  std::expected<std::size_t, std::string>
  wholeMember(const JsonValue& object, std::string_view key, std::optional<std::size_t> fallback) {
    const JsonValue* value = object.find(key);
    if (value == nullptr && fallback) {
      return *fallback;
    }
    const std::optional<std::size_t> number = wholeNumber(value);
    if (!number) {
      return std::unexpected(std::format("the member \"{}\" is missing or not a whole number", key));
    }
    return *number;
  }

  std::expected<JsonValue, std::string> parseJson(std::string_view text, const JsonLimits& limits) {
    return Parser(text, limits).run();
  }
} // namespace luciole::formats
