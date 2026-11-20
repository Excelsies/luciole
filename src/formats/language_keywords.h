#pragma once

#include "formats/file_kind.h"

#include <span>
#include <string_view>

// The words each language's tokenizer reads as keywords.
namespace luciole::formats {
  // Words that tokenize as keywords, sorted by byte value. Caseless sets hold lowercase words and match any ASCII
  // case.
  struct KeywordSet {
    std::span<const std::string_view> words;
    bool caseless = false;
  };

  // Empty for languages without keywords.
  KeywordSet keywords(Language language);
} // namespace luciole::formats
