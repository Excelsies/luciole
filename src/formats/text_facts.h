#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace luciole::formats {
  enum class LineEnding : std::uint8_t { None, Lf, CrLf, Cr };

  // What an editor must keep to write a text file back byte for byte.
  struct TextFacts {
    bool validUtf8 = true;
    bool byteOrderMark = false;               // starts with EF BB BF
    LineEnding lineEnding = LineEnding::None; // the most common; ties prefer LF, then CRLF, then CR
    bool mixedEndings = false;                // more than one style appears
    bool finalNewline = false;                // the last byte is LF or CR
    bool operator==(const TextFacts&) const = default;
  };

  // One pass over bytes. A CR followed by LF is one CRLF; any other CR is a CR ending. UTF-8 is strict: no
  // overlong forms, surrogates, values above U+10FFFF or truncated sequences.
  TextFacts textFacts(std::string_view bytes);

  // Editor text: a leading byte order mark removed, every CRLF and CR made LF. Other bytes are copied.
  std::string editorText(std::string_view bytes);

  // File bytes from editor text: each LF becomes the recorded ending (LF when there is none) and the mark is
  // restored. editorText followed by fileBytes reproduces the original bytes whenever mixedEndings is false.
  std::string fileBytes(std::string_view text, const TextFacts& facts);
} // namespace luciole::formats
