#include "formats/xml_reader.h"

#include "formats/utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <utility>

namespace luciole::formats {
  namespace {
    constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";
    constexpr std::string_view kComment = "<!--";
    constexpr std::string_view kCData = "<![CDATA[";
    constexpr std::string_view kDoctype = "<!DOCTYPE";
    // Longest reference body between '&' and ';' that is looked at.
    constexpr std::size_t kMaxReferenceBytes = 32;
    // Most digits in a numeric character reference.
    constexpr std::size_t kMaxReferenceDigits = 8;

    enum class Decode : std::uint8_t { Text, Attribute, CData };
    enum class Match : std::uint8_t { Yes, Partial, No };

    bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    bool endsName(char c) {
      return isSpace(c) || c == '/' || c == '>' || c == '=' || c == '<' || c == '"' || c == '\'' || c == '&';
    }

    // Whether `rest` starts with `token`, or ends before it could tell.
    Match matchToken(std::string_view rest, std::string_view token) {
      if (rest.starts_with(token)) {
        return Match::Yes;
      }
      return token.starts_with(rest) ? Match::Partial : Match::No;
    }

    bool allowedCharacter(std::uint32_t code) {
      return code == 0x9
          || code == 0xA
          || code == 0xD
          || (code >= 0x20 && code <= 0xD7FF)
          || (code >= 0xE000 && code <= 0xFFFD)
          || (code >= 0x10000 && code <= 0x10FFFF);
    }

    XmlEvent elementEvent(XmlEventKind kind, std::string_view name, std::span<const XmlAttribute> attributes = {}) {
      return {.kind = kind, .name = name, .attributes = attributes, .text = {}};
    }

    XmlEvent textEvent(XmlEventKind kind, std::string_view text) {
      return {.kind = kind, .name = {}, .attributes = {}, .text = text};
    }

    // Appends the character a reference body (between '&' and ';') names.
    std::expected<void, XmlError> appendReference(std::string_view body, std::string& out) {
      if (!body.starts_with('#')) {
        using Entity = std::pair<std::string_view, char>;
        constexpr std::array<Entity, 5> kEntities = {{
            {"lt", '<'},
            {"gt", '>'},
            {"amp", '&'},
            {"apos", '\''},
            {"quot", '"'},
        }};
        const auto* const entity = std::ranges::find(kEntities, body, &Entity::first);
        if (entity == kEntities.end()) {
          return std::unexpected(XmlError::UnknownEntity);
        }
        out += entity->second;
        return {};
      }
      const bool hex = body.size() > 1 && body[1] == 'x';
      const std::string_view digits = body.substr(hex ? 2 : 1);
      if (digits.empty() || digits.size() > kMaxReferenceDigits) {
        return std::unexpected(XmlError::BadCharacterReference);
      }
      std::uint32_t code = 0;
      const auto [end, status] = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
      if (status != std::errc{} || end != digits.data() + digits.size() || !allowedCharacter(code)) {
        return std::unexpected(XmlError::BadCharacterReference);
      }
      appendUtf8(out, code);
      return {};
    }

    // Appends `raw` to `out` with line endings normalized and, outside CDATA, references decoded. Each pass
    // consumes at least one byte of `raw`.
    std::expected<void, XmlError>
    decodeInto(std::string_view raw, std::string& out, Decode mode, std::size_t maxBytes) {
      const std::size_t start = out.size();
      const std::string_view special = mode == Decode::Attribute ? "&\r\t\n" : (mode == Decode::Text ? "&\r" : "\r");
      std::size_t i = 0;
      while (i < raw.size()) {
        const std::size_t stop = std::min(raw.find_first_of(special, i), raw.size());
        out.append(raw.substr(i, stop - i));
        i = stop;
        if (i < raw.size()) {
          const char c = raw[i];
          if (c == '&') {
            const std::size_t semicolon = raw.substr(i + 1, kMaxReferenceBytes + 1).find(';');
            if (semicolon == std::string_view::npos) {
              return std::unexpected(XmlError::Malformed);
            }
            if (auto added = appendReference(raw.substr(i + 1, semicolon), out); !added) {
              return added;
            }
            i += semicolon + 2;
          } else if (c == '\r') {
            out += mode == Decode::Attribute ? ' ' : '\n';
            i += i + 1 < raw.size() && raw[i + 1] == '\n' ? 2 : 1;
          } else {
            out += ' ';
            ++i;
          }
        }
        if (out.size() - start > maxBytes) {
          return std::unexpected(XmlError::ValueTooLong);
        }
      }
      return {};
    }
  } // namespace

  XmlReader::XmlReader(std::string_view bytes, const XmlLimits& limits)
      : m_bytes(bytes), m_limits(limits), m_pos(bytes.starts_with(kByteOrderMark) ? kByteOrderMark.size() : 0) {}

  std::expected<XmlEvent, XmlError> XmlReader::next() {
    if (m_error) {
      return std::unexpected(*m_error);
    }
    std::expected<XmlEvent, XmlError> event;
    if (m_closeSelf) {
      m_closeSelf = false;
      event = elementEvent(XmlEventKind::EndElement, m_open.back());
      m_open.pop_back();
    } else {
      event = readEvent();
    }
    if (event && event->kind != XmlEventKind::End && ++m_events > m_limits.maxEvents) {
      event = std::unexpected(XmlError::TooManyEvents);
    }
    if (!event) {
      m_error = event.error();
    }
    return event;
  }

  // Each pass consumes at least one byte or returns, so the loop ends within the input.
  std::expected<XmlEvent, XmlError> XmlReader::readEvent() {
    while (m_pos < m_bytes.size()) {
      if (m_bytes[m_pos] != '<') {
        if (!m_open.empty()) {
          return readText();
        }
        if (!isSpace(m_bytes[m_pos])) {
          return std::unexpected(XmlError::Malformed);
        }
        ++m_pos;
        continue;
      }
      auto markup = readMarkup();
      if (!markup) {
        return std::unexpected(markup.error());
      }
      if (*markup) {
        return **markup;
      }
    }
    if (!m_open.empty() || !m_rootSeen) {
      return std::unexpected(XmlError::Unterminated);
    }
    return XmlEvent{};
  }

  std::expected<XmlEvent, XmlError> XmlReader::readText() {
    const std::size_t end = m_bytes.find('<', m_pos);
    if (end == std::string_view::npos) {
      return std::unexpected(XmlError::Unterminated);
    }
    m_text.clear();
    if (auto decoded = decodeInto(m_bytes.substr(m_pos, end - m_pos), m_text, Decode::Text, m_limits.maxValueBytes);
        !decoded) {
      return std::unexpected(decoded.error());
    }
    m_pos = end;
    return textEvent(XmlEventKind::Text, m_text);
  }

  std::expected<std::optional<XmlEvent>, XmlError> XmlReader::readMarkup() {
    const std::string_view rest = m_bytes.substr(m_pos);
    if (rest.size() < 2) {
      return std::unexpected(XmlError::Unterminated);
    }
    if (rest[1] == '/') {
      return readEndTag();
    }
    if (rest[1] == '?') {
      const std::size_t end = rest.find("?>", 2);
      if (end == std::string_view::npos) {
        return std::unexpected(XmlError::Unterminated);
      }
      m_pos += end + 2;
      return std::nullopt;
    }
    if (rest[1] != '!') {
      return readStartTag();
    }
    const Match comment = matchToken(rest, kComment);
    const Match cdata = matchToken(rest, kCData);
    const Match doctype = matchToken(rest, kDoctype);
    if (comment == Match::Yes) {
      const std::size_t end = rest.find("-->", kComment.size());
      if (end == std::string_view::npos) {
        return std::unexpected(XmlError::Unterminated);
      }
      m_pos += end + 3;
      return std::nullopt;
    }
    if (cdata == Match::Yes) {
      if (m_open.empty()) {
        return std::unexpected(XmlError::Malformed);
      }
      const std::size_t end = rest.find("]]>", kCData.size());
      if (end == std::string_view::npos) {
        return std::unexpected(XmlError::Unterminated);
      }
      m_text.clear();
      const std::string_view raw = rest.substr(kCData.size(), end - kCData.size());
      if (auto decoded = decodeInto(raw, m_text, Decode::CData, m_limits.maxValueBytes); !decoded) {
        return std::unexpected(decoded.error());
      }
      m_pos += end + 3;
      return textEvent(XmlEventKind::CData, m_text);
    }
    if (doctype == Match::Yes) {
      if (auto skipped = skipDoctype(); !skipped) {
        return std::unexpected(skipped.error());
      }
      return std::nullopt;
    }
    if (comment == Match::Partial || cdata == Match::Partial || doctype == Match::Partial) {
      return std::unexpected(XmlError::Unterminated);
    }
    return std::unexpected(XmlError::Malformed);
  }

  std::expected<XmlEvent, XmlError> XmlReader::readStartTag() {
    if (m_open.empty() && m_rootSeen) {
      return std::unexpected(XmlError::Malformed);
    }
    ++m_pos;
    const auto name = readName();
    if (!name) {
      return std::unexpected(name.error());
    }
    if (m_open.size() >= m_limits.maxDepth) {
      return std::unexpected(XmlError::TooDeep);
    }
    if (auto attributes = readAttributes(); !attributes) {
      return std::unexpected(attributes.error());
    }
    m_open.push_back(*name);
    m_rootSeen = true;
    return elementEvent(XmlEventKind::StartElement, *name, m_attributes);
  }

  std::expected<XmlEvent, XmlError> XmlReader::readEndTag() {
    m_pos += 2;
    const auto name = readName();
    if (!name) {
      return std::unexpected(name.error());
    }
    skipSpaces();
    if (m_pos >= m_bytes.size()) {
      return std::unexpected(XmlError::Unterminated);
    }
    if (m_bytes[m_pos] != '>') {
      return std::unexpected(XmlError::Malformed);
    }
    ++m_pos;
    if (m_open.empty() || m_open.back() != *name) {
      return std::unexpected(XmlError::MismatchedTag);
    }
    m_open.pop_back();
    return elementEvent(XmlEventKind::EndElement, *name);
  }

  // Reads attributes up to and including '>' or "/>". Each pass consumes at least the attribute's name and
  // quotes, and passes are capped by maxAttributes.
  std::expected<void, XmlError> XmlReader::readAttributes() {
    m_attributes.clear();
    m_valueEnds.clear();
    m_values.clear();
    while (m_pos < m_bytes.size()) {
      const bool spaced = skipSpaces();
      if (m_pos >= m_bytes.size()) {
        break;
      }
      const char c = m_bytes[m_pos];
      if (c == '>' || c == '/') {
        if (c == '/' && m_pos + 1 >= m_bytes.size()) {
          break;
        }
        if (c == '/' && m_bytes[m_pos + 1] != '>') {
          return std::unexpected(XmlError::Malformed);
        }
        m_closeSelf = c == '/';
        m_pos += m_closeSelf ? 2 : 1;
        std::size_t begin = 0;
        for (std::size_t k = 0; k < m_attributes.size(); ++k) {
          m_attributes[k].value = std::string_view(m_values).substr(begin, m_valueEnds[k] - begin);
          begin = m_valueEnds[k];
        }
        return {};
      }
      if (!spaced) {
        return std::unexpected(XmlError::Malformed);
      }
      if (m_attributes.size() >= m_limits.maxAttributes) {
        return std::unexpected(XmlError::TooManyAttributes);
      }
      const auto name = readName();
      if (!name) {
        return std::unexpected(name.error());
      }
      skipSpaces();
      if (m_pos >= m_bytes.size()) {
        break;
      }
      if (m_bytes[m_pos] != '=') {
        return std::unexpected(XmlError::Malformed);
      }
      ++m_pos;
      skipSpaces();
      if (m_pos >= m_bytes.size()) {
        break;
      }
      const char quote = m_bytes[m_pos];
      if (quote != '"' && quote != '\'') {
        return std::unexpected(XmlError::Malformed);
      }
      const std::size_t end = m_bytes.find(quote, m_pos + 1);
      if (end == std::string_view::npos) {
        break;
      }
      const std::string_view raw = m_bytes.substr(m_pos + 1, end - m_pos - 1);
      if (raw.contains('<')) {
        return std::unexpected(XmlError::Malformed);
      }
      if (auto decoded = decodeInto(raw, m_values, Decode::Attribute, m_limits.maxValueBytes); !decoded) {
        return decoded;
      }
      m_attributes.push_back({.name = *name, .value = {}});
      m_valueEnds.push_back(m_values.size());
      m_pos = end + 1;
    }
    return std::unexpected(XmlError::Unterminated);
  }

  // Skips the DOCTYPE at the read position without reading its declarations: quoted strings, and comments
  // and processing instructions in the internal subset, are passed over so a '>' inside them does not end it.
  // Each pass advances at least one byte.
  std::expected<void, XmlError> XmlReader::skipDoctype() {
    if (m_rootSeen || m_doctypeSeen) {
      return std::unexpected(XmlError::Malformed);
    }
    m_doctypeSeen = true;
    std::size_t i = m_pos + kDoctype.size();
    char quote = 0;
    std::size_t brackets = 0;
    while (i < m_bytes.size()) {
      const char c = m_bytes[i];
      const std::string_view rest = m_bytes.substr(i);
      std::size_t skip = 1;
      if (quote != 0) {
        quote = c == quote ? 0 : quote;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else if (brackets > 0 && (rest.starts_with(kComment) || rest.starts_with("<?"))) {
        const std::string_view close = rest.starts_with(kComment) ? "-->" : "?>";
        const std::size_t end = rest.find(close, 2);
        if (end == std::string_view::npos) {
          break;
        }
        skip = end + close.size();
      } else if (c == '[') {
        ++brackets;
      } else if (c == ']' && brackets > 0) {
        --brackets;
      } else if (c == '>' && brackets == 0) {
        m_pos = i + 1;
        return {};
      }
      i += skip;
    }
    return std::unexpected(XmlError::Unterminated);
  }

  std::expected<std::string_view, XmlError> XmlReader::readName() {
    const std::size_t start = m_pos;
    const std::size_t stop = std::min(m_bytes.size(), start + m_limits.maxNameBytes + 1);
    while (m_pos < stop && !endsName(m_bytes[m_pos])) {
      ++m_pos;
    }
    if (m_pos - start > m_limits.maxNameBytes) {
      return std::unexpected(XmlError::NameTooLong);
    }
    if (m_pos >= m_bytes.size()) {
      return std::unexpected(XmlError::Unterminated);
    }
    if (m_pos == start) {
      return std::unexpected(XmlError::Malformed);
    }
    return m_bytes.substr(start, m_pos - start);
  }

  bool XmlReader::skipSpaces() {
    const std::size_t start = m_pos;
    while (m_pos < m_bytes.size() && isSpace(m_bytes[m_pos])) {
      ++m_pos;
    }
    return m_pos > start;
  }

  std::string_view localName(std::string_view name) {
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
  }

  std::string_view prefix(std::string_view name) {
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? std::string_view{} : name.substr(0, colon);
  }

  bool hasLocalName(std::string_view name, std::string_view local) { return localName(name) == local; }

  std::optional<std::string_view> attributeValue(std::span<const XmlAttribute> attributes, std::string_view name) {
    const auto found = std::ranges::find(attributes, name, &XmlAttribute::name);
    return found == attributes.end() ? std::nullopt : std::optional(found->value);
  }

  std::optional<std::string_view>
  attributeValueByLocalName(std::span<const XmlAttribute> attributes, std::string_view local) {
    const auto found = std::ranges::find_if(attributes, [local](const XmlAttribute& attribute) {
      return hasLocalName(attribute.name, local);
    });
    return found == attributes.end() ? std::nullopt : std::optional(found->value);
  }
} // namespace luciole::formats
