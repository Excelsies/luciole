#pragma once

#include <cstdint>
#include <string_view>

namespace luciole::formats {
  // What a file holds, as far as showing it goes.
  enum class ContentKind : std::uint8_t { Other, Text, Image, Video, Audio };

  // Where a line comment starts: "//", "#", "--", or ";" and "#". Hash and Semi comments need the start of the line
  // or a space or tab before them.
  enum class CommentStyle : std::uint8_t { None, Slash, Hash, Dash, Semi };

  enum class Language : std::uint8_t {
    None, // not text
    Plain,
    Markdown,
    Json,
    JsonLines,
    Yaml,
    Toml,
    Ini,
    DesktopEntry,
    Xml,
    Html,
    Css,
    Scss,
    Csv,
    Tsv,
    Diff,
    Code, // code without a keyword set of its own
    C,
    Cpp,
    CSharp,
    Java,
    Kotlin,
    EcmaScript, // .js, .mjs, .cjs, .jsx
    TypeScript,
    Rust,
    Go,
    Swift,
    Zig,
    Php,
    Python,
    Ruby,
    Lua,
    Shell,
    Fish,
    Nix,
    Haskell,
    Sql,
    Meson,
    CMake,
    Make,
    Dockerfile
  };

  // Which tokenizer reads a language. Markdown and Delimited have readers of their own and tokenize as plain lines.
  enum class TextSyntax : std::uint8_t {
    None,
    Plain,
    Markdown,
    Delimited,
    Json,
    JsonLines,
    Yaml,
    Toml,
    Ini,
    Xml,
    Code,
    Diff
  };

  struct FileKind {
    ContentKind content = ContentKind::Other;
    Language language = Language::None;
    CommentStyle comment = CommentStyle::None;
    bool prose = false; // plain text meant to be read, which a preview wraps to its width
    bool operator==(const FileKind&) const = default;
  };

  // From the name alone, ignoring ASCII case: an exact name ("Makefile"), then the extension after the last dot when
  // that dot is neither the first nor the last character, then a known prefix followed by '-', '_' or '.'
  // ("LICENSE-MIT", ".env.local", "Dockerfile.dev"). Unknown names give the default FileKind.
  FileKind fileKind(std::string_view name);

  // None for Language::None and for values outside the enum.
  TextSyntax textSyntax(Language language);
} // namespace luciole::formats
