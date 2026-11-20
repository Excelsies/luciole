#include "formats/file_kind.h"

#include "formats/ascii.h"

#include <algorithm>
#include <array>
#include <string>

namespace luciole::formats {
  namespace {
    struct KindWords {
      std::string_view words; // space separated, lowercase
      FileKind kind;
    };

    struct KindPrefix {
      std::string_view prefix;
      FileKind kind;
    };

    constexpr FileKind media(ContentKind content) { return {.content = content}; }

    constexpr FileKind text(Language language, CommentStyle comment = CommentStyle::None) {
      return {.content = ContentKind::Text, .language = language, .comment = comment};
    }

    constexpr FileKind prose() {
      return {.content = ContentKind::Text, .language = Language::Plain, .comment = CommentStyle::None, .prose = true};
    }

    using enum Language;
    constexpr auto kSlash = CommentStyle::Slash;
    constexpr auto kHash = CommentStyle::Hash;
    constexpr auto kDash = CommentStyle::Dash;
    constexpr auto kSemi = CommentStyle::Semi;

    constexpr std::array kExtensions{
        KindWords{"png jpg jpeg gif webp svg bmp ico avif apng", media(ContentKind::Image)},
        KindWords{"mp4 m4v webm ogv mkv mov bik bk2", media(ContentKind::Video)},
        KindWords{"ogg oga opus mp3 flac wav m4a aac", media(ContentKind::Audio)},
        KindWords{"json geojson webmanifest", text(Json)},
        KindWords{"jsonl ndjson", text(JsonLines)},
        KindWords{"md markdown mdown mdx", text(Markdown)},
        KindWords{"yaml yml", text(Yaml, kHash)},
        KindWords{"toml", text(Toml, kHash)},
        KindWords{"ini cfg conf service socket timer properties editorconfig gitconfig repo env", text(Ini, kSemi)},
        KindWords{"desktop", text(DesktopEntry, kSemi)},
        KindWords{"xml xsl xslt plist ui qrc kml rss atom xsd", text(Xml)},
        KindWords{"html htm xhtml", text(Html)},
        KindWords{"css", text(Css, kSlash)},
        KindWords{"scss sass less", text(Scss, kSlash)},
        KindWords{"c h", text(C, kSlash)},
        KindWords{"cc cpp cxx c++ hpp hh hxx h++ ipp tpp inl", text(Cpp, kSlash)},
        KindWords{"cs", text(CSharp, kSlash)},
        KindWords{"java", text(Java, kSlash)},
        KindWords{"kt kts", text(Kotlin, kSlash)},
        KindWords{"js mjs cjs jsx", text(EcmaScript, kSlash)},
        KindWords{"ts mts cts tsx", text(TypeScript, kSlash)},
        KindWords{"rs", text(Rust, kSlash)},
        KindWords{"go", text(Go, kSlash)},
        KindWords{"swift", text(Swift, kSlash)},
        KindWords{"zig zon", text(Zig, kSlash)},
        KindWords{"php", text(Php, kSlash)},
        KindWords{"py pyi pyw", text(Python, kHash)},
        KindWords{"rb rake gemspec", text(Ruby, kHash)},
        KindWords{"sh bash zsh ksh", text(Shell, kHash)},
        KindWords{"fish", text(Fish, kHash)},
        KindWords{"nix", text(Nix, kHash)},
        KindWords{"cmake", text(CMake, kHash)},
        KindWords{"mk mak", text(Make, kHash)},
        KindWords{"dockerfile containerfile", text(Dockerfile, kHash)},
        KindWords{"lua", text(Lua, kDash)},
        KindWords{"sql", text(Sql, kDash)},
        KindWords{"hs", text(Haskell, kDash)},
        KindWords{"qml glsl vert frag scala dart proto gradle jsonc json5 vue svelte", text(Code, kSlash)},
        KindWords{"pl pm r tcl ps1 awk bat cmd", text(Code, kHash)},
        KindWords{"elm", text(Code, kDash)},
        KindWords{"diff patch", text(Diff)},
        KindWords{"csv", text(Csv)},
        KindWords{"tsv", text(Tsv)},
        KindWords{"txt text", prose()},
        KindWords{"log rst nfo tex lock", text(Plain)},
    };

    constexpr std::array kNames{
        KindWords{"license copying readme authors changelog notice contributors todo version", prose()},
        KindWords{".gitignore .gitattributes .dockerignore .gitmodules", text(Plain)},
        KindWords{"makefile gnumakefile", text(Make, kHash)},
        KindWords{"justfile", text(Code, kHash)},
        KindWords{"dockerfile containerfile", text(Dockerfile, kHash)},
        KindWords{
            "pkgbuild .bashrc .zshrc .profile .bash_profile .bash_logout .bash_aliases .zprofile .zshenv",
            text(Shell, kHash)
        },
        KindWords{"meson.build meson.options meson_options.txt", text(Meson, kHash)},
        KindWords{"cmakelists.txt", text(CMake, kHash)},
        KindWords{"gemfile rakefile vagrantfile", text(Ruby, kHash)},
        KindWords{"go.mod tsconfig.json jsconfig.json .eslintrc.json .babelrc", text(Code, kSlash)},
        KindWords{".editorconfig .gitconfig .env", text(Ini, kSemi)},
        KindWords{"cargo.lock", text(Toml, kHash)},
        KindWords{"flake.lock", text(Json)},
        KindWords{".clang-format .clang-tidy", text(Yaml, kHash)},
    };

    constexpr std::array kPrefixes{
        KindPrefix{"license", prose()},
        KindPrefix{"licence", prose()},
        KindPrefix{"copying", prose()},
        KindPrefix{"readme", prose()},
        KindPrefix{"authors", prose()},
        KindPrefix{"changelog", prose()},
        KindPrefix{"notice", prose()},
        KindPrefix{".env", text(Ini, kSemi)},
        KindPrefix{"dockerfile", text(Dockerfile, kHash)},
        KindPrefix{"containerfile", text(Dockerfile, kHash)},
        KindPrefix{"makefile", text(Make, kHash)},
    };

    // Whether `word` is one of the space separated words in `list`.
    bool listed(std::string_view list, std::string_view word) {
      // Each step moves past one word, so the loop runs at most list.size() times.
      while (!list.empty()) {
        const auto space = list.find(' ');
        if (list.substr(0, space) == word) {
          return true;
        }
        list = space == std::string_view::npos ? std::string_view{} : list.substr(space + 1);
      }
      return false;
    }

    template <std::size_t N> const FileKind* findListed(const std::array<KindWords, N>& table, std::string_view word) {
      if (word.empty()) {
        return nullptr;
      }
      const auto* const found =
          std::ranges::find_if(table, [&](const KindWords& row) { return listed(row.words, word); });
      return found == table.end() ? nullptr : &found->kind;
    }

    const FileKind* findPrefix(std::string_view lower) {
      const auto* const found = std::ranges::find_if(kPrefixes, [&](const KindPrefix& row) {
        return lower.starts_with(row.prefix)
            && (lower.size() == row.prefix.size() || std::string_view{"-_."}.contains(lower[row.prefix.size()]));
      });
      return found == kPrefixes.end() ? nullptr : &found->kind;
    }

    std::string_view extensionOf(std::string_view lower) {
      const auto dot = lower.rfind('.');
      if (dot == std::string_view::npos || dot == 0 || dot + 1 >= lower.size()) {
        return {};
      }
      return lower.substr(dot + 1);
    }
  } // namespace

  FileKind fileKind(std::string_view name) {
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), toAsciiLower);
    if (const auto* const kind = findListed(kNames, lower)) {
      return *kind;
    }
    if (const auto* const kind = findListed(kExtensions, extensionOf(lower))) {
      return *kind;
    }
    if (const auto* const kind = findPrefix(lower)) {
      return *kind;
    }
    return {};
  }

  TextSyntax textSyntax(Language language) {
    switch (language) {
    case Language::None:
      return TextSyntax::None;
    case Language::Plain:
      return TextSyntax::Plain;
    case Language::Markdown:
      return TextSyntax::Markdown;
    case Language::Csv:
    case Language::Tsv:
      return TextSyntax::Delimited;
    case Language::Json:
      return TextSyntax::Json;
    case Language::JsonLines:
      return TextSyntax::JsonLines;
    case Language::Yaml:
      return TextSyntax::Yaml;
    case Language::Toml:
      return TextSyntax::Toml;
    case Language::Ini:
    case Language::DesktopEntry:
      return TextSyntax::Ini;
    case Language::Xml:
    case Language::Html:
      return TextSyntax::Xml;
    case Language::Diff:
      return TextSyntax::Diff;
    case Language::Css:
    case Language::Scss:
    case Language::Code:
    case Language::C:
    case Language::Cpp:
    case Language::CSharp:
    case Language::Java:
    case Language::Kotlin:
    case Language::EcmaScript:
    case Language::TypeScript:
    case Language::Rust:
    case Language::Go:
    case Language::Swift:
    case Language::Zig:
    case Language::Php:
    case Language::Python:
    case Language::Ruby:
    case Language::Lua:
    case Language::Shell:
    case Language::Fish:
    case Language::Nix:
    case Language::Haskell:
    case Language::Sql:
    case Language::Meson:
    case Language::CMake:
    case Language::Make:
    case Language::Dockerfile:
      return TextSyntax::Code;
    }
    return TextSyntax::None;
  }

} // namespace luciole::formats
