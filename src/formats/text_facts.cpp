#include "formats/text_facts.h"

#include <algorithm>
#include <cstddef>

namespace {
  constexpr std::string_view kMark = "\xEF\xBB\xBF";

  std::size_t contentStart(std::string_view bytes) { return bytes.starts_with(kMark) ? kMark.size() : 0; }

  bool between(unsigned char value, unsigned char low, unsigned char high) { return value >= low && value <= high; }

  // Length of the well-formed UTF-8 sequence that starts with a non-ASCII byte at `at`, or 0 when it is not one.
  std::size_t sequenceLength(std::string_view bytes, std::size_t at) {
    const auto lead = static_cast<unsigned char>(bytes[at]);
    std::size_t length = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (between(lead, 0xC2, 0xDF)) {
      length = 2;
    } else if (lead == 0xE0) {
      length = 3;
      low = 0xA0;
    } else if (lead == 0xED) {
      length = 3;
      high = 0x9F;
    } else if (between(lead, 0xE1, 0xEF)) {
      length = 3;
    } else if (lead == 0xF0) {
      length = 4;
      low = 0x90;
    } else if (between(lead, 0xF1, 0xF3)) {
      length = 4;
    } else if (lead == 0xF4) {
      length = 4;
      high = 0x8F;
    } else {
      return 0;
    }
    if (bytes.size() - at < length || !between(static_cast<unsigned char>(bytes[at + 1]), low, high)) {
      return 0;
    }
    for (std::size_t k = 2; k < length; ++k) {
      if (!between(static_cast<unsigned char>(bytes[at + k]), 0x80, 0xBF)) {
        return 0;
      }
    }
    return length;
  }

  std::string_view endingBytes(luciole::formats::LineEnding ending) {
    using luciole::formats::LineEnding;
    switch (ending) {
    case LineEnding::CrLf:
      return "\r\n";
    case LineEnding::Cr:
      return "\r";
    case LineEnding::None:
    case LineEnding::Lf:
      break;
    }
    return "\n";
  }
} // namespace

luciole::formats::TextFacts luciole::formats::textFacts(std::string_view bytes) {
  TextFacts facts;
  const auto start = contentStart(bytes);
  facts.byteOrderMark = start > 0;
  std::size_t lf = 0;
  std::size_t crlf = 0;
  std::size_t cr = 0;
  // Bounded by bytes.size(): every step advances i by at least one.
  std::size_t i = start;
  while (i < bytes.size()) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    if (byte == '\n') {
      ++lf;
      ++i;
    } else if (byte == '\r') {
      const bool pair = i + 1 < bytes.size() && bytes[i + 1] == '\n';
      ++(pair ? crlf : cr);
      i += pair ? 2 : 1;
    } else if (byte < 0x80 || !facts.validUtf8) {
      ++i;
    } else if (const auto length = sequenceLength(bytes, i); length > 0) {
      i += length;
    } else {
      facts.validUtf8 = false;
      ++i;
    }
  }
  const auto styles = static_cast<int>(lf > 0) + static_cast<int>(crlf > 0) + static_cast<int>(cr > 0);
  facts.mixedEndings = styles > 1;
  if (styles > 0) {
    const auto most = std::max({lf, crlf, cr});
    facts.lineEnding = lf == most ? LineEnding::Lf : (crlf == most ? LineEnding::CrLf : LineEnding::Cr);
  }
  facts.finalNewline = bytes.size() > start && (bytes.back() == '\n' || bytes.back() == '\r');
  return facts;
}

std::string luciole::formats::editorText(std::string_view bytes) {
  const auto content = bytes.substr(contentStart(bytes));
  std::string text;
  text.reserve(content.size());
  // Bounded by content.size().
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r') {
      text.push_back(content[i]);
      continue;
    }
    text.push_back('\n');
    if (i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
  }
  return text;
}

std::string luciole::formats::fileBytes(std::string_view text, const TextFacts& facts) {
  const auto ending = endingBytes(facts.lineEnding);
  const auto lines = static_cast<std::size_t>(std::ranges::count(text, '\n'));
  std::string bytes;
  bytes.reserve(text.size() + (lines * (ending.size() - 1)) + (facts.byteOrderMark ? kMark.size() : 0));
  if (facts.byteOrderMark) {
    bytes.append(kMark);
  }
  // Bounded by text.size().
  for (const char byte : text) {
    if (byte == '\n') {
      bytes.append(ending);
    } else {
      bytes.push_back(byte);
    }
  }
  return bytes;
}
