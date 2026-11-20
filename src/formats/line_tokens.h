#pragma once

#include "formats/code_tokens.h"
#include "formats/file_kind.h"
#include "formats/text_lines.h"

// The line tokenizers of formatText: YAML, TOML, INI, XML, JSON Lines, diffs and code.
namespace luciole::formats::tokenize {
  // Tokens of one line in `syntax`, or one plain token for a syntax without a line tokenizer. `state` carries what
  // a line leaves open into the next one.
  void tokenizeLine(const LineWriter& w, TextSyntax syntax, const CodeOptions& code, ScanState& state);
} // namespace luciole::formats::tokenize
