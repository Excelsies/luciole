#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::formats {
  enum class XmlEventKind : std::uint8_t { StartElement, EndElement, Text, CData, End };

  enum class XmlError : std::uint8_t {
    Unterminated,          // the input ends inside markup, a reference, or before the root element closes
    Malformed,             // markup that is not well-formed XML
    MismatchedTag,         // an end tag that does not close the open element
    UnknownEntity,         // a named reference other than the five predefined entities
    BadCharacterReference, // a numeric reference that is empty, too long, or names a character XML does not allow
    TooDeep,
    TooManyAttributes,
    NameTooLong,
    ValueTooLong,
    TooManyEvents,
  };

  struct XmlLimits {
    std::size_t maxDepth = 256;                  // open elements
    std::size_t maxAttributes = 256;             // per element
    std::size_t maxNameBytes = 1024;             // element and attribute names, prefix included
    std::size_t maxValueBytes = 4 * 1024 * 1024; // one decoded attribute value, text run or CDATA section
    std::size_t maxEvents = 16 * 1024 * 1024;    // events before End
  };

  struct XmlAttribute {
    std::string_view name;  // as written, prefix included
    std::string_view value; // decoded
  };

  // Views point into the input or into the reader and stay valid until the next call to next().
  struct XmlEvent {
    XmlEventKind kind = XmlEventKind::End;
    std::string_view name;                    // StartElement and EndElement, prefix included
    std::span<const XmlAttribute> attributes; // StartElement
    std::string_view text;                    // Text and CData, decoded
  };

  // Pull parser over one document held in memory. A self-closing element gives a StartElement and an
  // EndElement. Comments and processing instructions are skipped. A DOCTYPE is skipped without reading its
  // declarations, so the only entities are the five predefined ones. Text is reported inside the root element
  // only, whitespace included; CR and CRLF read as LF, and literal tabs and line endings in attribute values
  // as spaces. The first error is returned again by every later call; End is returned again once reached.
  class XmlReader {
  public:
    explicit XmlReader(std::string_view bytes, const XmlLimits& limits = {});

    std::expected<XmlEvent, XmlError> next();

    // Open elements after the last event; a StartElement counts its own element.
    std::size_t depth() const { return m_open.size(); }

  private:
    std::expected<XmlEvent, XmlError> readEvent();
    std::expected<XmlEvent, XmlError> readText();
    // An event, or nothing for a comment, processing instruction or DOCTYPE that was skipped.
    std::expected<std::optional<XmlEvent>, XmlError> readMarkup();
    std::expected<XmlEvent, XmlError> readStartTag();
    std::expected<XmlEvent, XmlError> readEndTag();
    std::expected<void, XmlError> readAttributes();
    std::expected<void, XmlError> skipDoctype();
    std::expected<std::string_view, XmlError> readName();
    bool skipSpaces();

    std::string_view m_bytes;
    XmlLimits m_limits;
    std::size_t m_pos = 0;
    std::size_t m_events = 0;
    std::vector<std::string_view> m_open;
    std::vector<XmlAttribute> m_attributes;
    std::vector<std::size_t> m_valueEnds; // value k is m_values[m_valueEnds[k - 1], m_valueEnds[k])
    std::string m_values;
    std::string m_text;
    std::optional<XmlError> m_error;
    bool m_closeSelf = false;
    bool m_rootSeen = false;
    bool m_doctypeSeen = false;
  };

  // The part of a name after its first colon, or the whole name.
  std::string_view localName(std::string_view name);
  // The part of a name before its first colon, or empty.
  std::string_view prefix(std::string_view name);
  bool hasLocalName(std::string_view name, std::string_view local);

  // The value of the first attribute with this exact name, prefix included.
  std::optional<std::string_view> attributeValue(std::span<const XmlAttribute> attributes, std::string_view name);
  // The value of the first attribute with this local name, whatever its prefix.
  std::optional<std::string_view>
  attributeValueByLocalName(std::span<const XmlAttribute> attributes, std::string_view local);
} // namespace luciole::formats
