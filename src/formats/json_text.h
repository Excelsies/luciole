#pragma once

#include "formats/text_tokens.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// JSON checked against RFC 8259 and printed with two-space indentation.
namespace luciole::formats {
  // Whether text holds a whole value, or may stop anywhere inside one, as text cut at a read limit does.
  enum class JsonExtent : std::uint8_t { Whole, Start };

  // Why `text` is not one JSON value, or with Start not the start of one, with the line and column of the problem,
  // or that it nests deeper than kMaxJsonDepth; none when it is valid.
  std::optional<std::string> jsonProblem(std::string_view text, JsonExtent extent = JsonExtent::Whole);

  struct PrintedJson {
    std::vector<TextLine> lines; // at most kMaxTextLines
    bool capped = false;         // lines past kMaxTextLines were left out
    bool endsEarly = false;      // not capped, and the text stopped inside an object, array or string
  };

  // Text that jsonProblem accepts, whole or as a start, one value, key or closing bracket per line in source order.
  // Every token but Space is a range of the source; Space tokens stand for the indentation and the space after a
  // colon.
  PrintedJson printJson(std::string_view text);
} // namespace luciole::formats
