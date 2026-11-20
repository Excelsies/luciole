#include "formats/file_kind.h"
#include "test_check.h"

#include <algorithm>
#include <print>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::formats;

  TextSyntax syntaxOf(std::string_view name) { return textSyntax(fileKind(name).language); }

  void testExtensionsCaseNamesAndUnknown() {
    for (const auto* name : {"a.PNG", "a.gif", "a.svg"}) {
      TEST_CHECK(fileKind(name).content == ContentKind::Image);
    }
    TEST_CHECK(fileKind("clip.webm").content == ContentKind::Video);
    TEST_CHECK(fileKind("intro.bik").content == ContentKind::Video);
    TEST_CHECK(fileKind("INTRO.BK2").content == ContentKind::Video);
    TEST_CHECK(fileKind("tone.ogg").content == ContentKind::Audio);
    TEST_CHECK(fileKind("x.json").content == ContentKind::Text);
    TEST_CHECK(fileKind("x.json").language == Language::Json);
    TEST_CHECK(fileKind("README.md").language == Language::Markdown);
    TEST_CHECK(syntaxOf("a.yml") == TextSyntax::Yaml);
    TEST_CHECK(syntaxOf("a.toml") == TextSyntax::Toml);
    TEST_CHECK(syntaxOf("a.ini") == TextSyntax::Ini);
    TEST_CHECK(syntaxOf("a.xml") == TextSyntax::Xml);
    TEST_CHECK(syntaxOf("a.rs") == TextSyntax::Code);
    TEST_CHECK(syntaxOf("a.txt") == TextSyntax::Plain);
    for (const auto* name : {"LICENSE", "README", "VERSION", "COPYING"}) {
      TEST_CHECK(fileKind(name).content == ContentKind::Text);
      TEST_CHECK(fileKind(name).language == Language::Plain);
    }
    for (const auto* name : {"Makefile", "justfile", "meson.build", "Dockerfile", "CMakeLists.txt"}) {
      TEST_CHECK(syntaxOf(name) == TextSyntax::Code);
    }
    TEST_CHECK(fileKind("Makefile").comment == CommentStyle::Hash);
    TEST_CHECK(fileKind("main.c").comment == CommentStyle::Slash);
    TEST_CHECK((fileKind("blob.bin") == FileKind{}));
    TEST_CHECK((fileKind("noext") == FileKind{}));
    TEST_CHECK(fileKind("blob.bin").content == ContentKind::Other);
    TEST_CHECK(fileKind("blob.bin").language == Language::None);
    TEST_CHECK(fileKind(".gitignore").language == Language::Plain);
  }

  void testCommonNames() {
    struct Row {
      std::string_view name;
      TextSyntax syntax;
    };
    for (const auto& row : {
             Row{".env", TextSyntax::Ini},
             Row{".env.local", TextSyntax::Ini},
             Row{"data.jsonl", TextSyntax::JsonLines},
             Row{"App.vue", TextSyntax::Code},
             Row{"run.bat", TextSyntax::Code},
             Row{"page.mdx", TextSyntax::Markdown},
             Row{".bash_profile", TextSyntax::Code},
             Row{".gitmodules", TextSyntax::Plain},
             Row{"LICENSE-MIT", TextSyntax::Plain},
             Row{"README_zh", TextSyntax::Plain},
             Row{"tsconfig.json", TextSyntax::Code},
             Row{"settings.jsonc", TextSyntax::Code},
         }) {
      TEST_CHECK(fileKind(row.name).content == ContentKind::Text);
      TEST_CHECK(syntaxOf(row.name) == row.syntax);
    }
    TEST_CHECK(fileKind("licensed.bin").content == ContentKind::Other);
  }

  void testDeveloperLanguages() {
    struct Row {
      std::string_view name;
      Language language;
      CommentStyle comment;
    };
    for (const auto& row : {
             Row{"main.c", Language::C, CommentStyle::Slash},
             Row{"list.h", Language::C, CommentStyle::Slash},
             Row{"tree.cpp", Language::Cpp, CommentStyle::Slash},
             Row{"tree.cc", Language::Cpp, CommentStyle::Slash},
             Row{"tree.hpp", Language::Cpp, CommentStyle::Slash},
             Row{"lib.rs", Language::Rust, CommentStyle::Slash},
             Row{"main.go", Language::Go, CommentStyle::Slash},
             Row{"tool.py", Language::Python, CommentStyle::Hash},
             Row{"types.pyi", Language::Python, CommentStyle::Hash},
             Row{"app.js", Language::EcmaScript, CommentStyle::Slash},
             Row{"app.mjs", Language::EcmaScript, CommentStyle::Slash},
             Row{"view.jsx", Language::EcmaScript, CommentStyle::Slash},
             Row{"app.ts", Language::TypeScript, CommentStyle::Slash},
             Row{"view.tsx", Language::TypeScript, CommentStyle::Slash},
             Row{"Main.java", Language::Java, CommentStyle::Slash},
             Row{"Main.kt", Language::Kotlin, CommentStyle::Slash},
             Row{"build.gradle.kts", Language::Kotlin, CommentStyle::Slash},
             Row{"View.swift", Language::Swift, CommentStyle::Slash},
             Row{"Program.cs", Language::CSharp, CommentStyle::Slash},
             Row{"task.rb", Language::Ruby, CommentStyle::Hash},
             Row{"Gemfile", Language::Ruby, CommentStyle::Hash},
             Row{"Rakefile", Language::Ruby, CommentStyle::Hash},
             Row{"index.php", Language::Php, CommentStyle::Slash},
             Row{"init.lua", Language::Lua, CommentStyle::Dash},
             Row{"run.sh", Language::Shell, CommentStyle::Hash},
             Row{"run.bash", Language::Shell, CommentStyle::Hash},
             Row{".bashrc", Language::Shell, CommentStyle::Hash},
             Row{".zshrc", Language::Shell, CommentStyle::Hash},
             Row{"PKGBUILD", Language::Shell, CommentStyle::Hash},
             Row{"config.fish", Language::Fish, CommentStyle::Hash},
             Row{"flake.nix", Language::Nix, CommentStyle::Hash},
             Row{"main.zig", Language::Zig, CommentStyle::Slash},
             Row{"Main.hs", Language::Haskell, CommentStyle::Dash},
             Row{"schema.sql", Language::Sql, CommentStyle::Dash},
             Row{"site.css", Language::Css, CommentStyle::Slash},
             Row{"site.scss", Language::Scss, CommentStyle::Slash},
             Row{"index.html", Language::Html, CommentStyle::None},
             Row{"index.htm", Language::Html, CommentStyle::None},
             Row{"feed.xml", Language::Xml, CommentStyle::None},
             Row{"window.ui", Language::Xml, CommentStyle::None},
             Row{"meson.build", Language::Meson, CommentStyle::Hash},
             Row{"meson.options", Language::Meson, CommentStyle::Hash},
             Row{"meson_options.txt", Language::Meson, CommentStyle::Hash},
             Row{"CMakeLists.txt", Language::CMake, CommentStyle::Hash},
             Row{"deps.cmake", Language::CMake, CommentStyle::Hash},
             Row{"Makefile", Language::Make, CommentStyle::Hash},
             Row{"GNUmakefile", Language::Make, CommentStyle::Hash},
             Row{"Makefile.am", Language::Make, CommentStyle::Hash},
             Row{"rules.mk", Language::Make, CommentStyle::Hash},
             Row{"Dockerfile", Language::Dockerfile, CommentStyle::Hash},
             Row{"Dockerfile.dev", Language::Dockerfile, CommentStyle::Hash},
             Row{"Containerfile", Language::Dockerfile, CommentStyle::Hash},
             Row{"app.dockerfile", Language::Dockerfile, CommentStyle::Hash},
             Row{"luciole.desktop", Language::DesktopEntry, CommentStyle::Semi},
             Row{"fix.diff", Language::Diff, CommentStyle::None},
             Row{"fix.patch", Language::Diff, CommentStyle::None},
             Row{"package.json", Language::Json, CommentStyle::None},
             Row{"flake.lock", Language::Json, CommentStyle::None},
             Row{"events.ndjson", Language::JsonLines, CommentStyle::None},
             Row{"config.yaml", Language::Yaml, CommentStyle::Hash},
             Row{".clang-format", Language::Yaml, CommentStyle::Hash},
             Row{".clang-tidy", Language::Yaml, CommentStyle::Hash},
             Row{"Cargo.toml", Language::Toml, CommentStyle::Hash},
             Row{"Cargo.lock", Language::Toml, CommentStyle::Hash},
             Row{"setup.cfg", Language::Ini, CommentStyle::Semi},
             Row{"pacman.conf", Language::Ini, CommentStyle::Semi},
             Row{"app.service", Language::Ini, CommentStyle::Semi},
             Row{"notes.markdown", Language::Markdown, CommentStyle::None},
             Row{"table.csv", Language::Csv, CommentStyle::None},
             Row{"table.tsv", Language::Tsv, CommentStyle::None},
             Row{"notes.txt", Language::Plain, CommentStyle::None},
             Row{"build.log", Language::Plain, CommentStyle::None},
         }) {
      const auto kind = fileKind(row.name);
      TEST_CHECK(kind.content == ContentKind::Text);
      TEST_CHECK(kind.language == row.language);
      TEST_CHECK(kind.comment == row.comment);
    }
  }

  void testLookupOrder() {
    // Exact name beats extension: meson_options.txt is not plain text, CMakeLists.txt is not plain text.
    TEST_CHECK(fileKind("meson_options.txt").language == Language::Meson);
    // Extension beats prefix: README.md is Markdown, not the plain README prefix.
    TEST_CHECK(fileKind("README.md").language == Language::Markdown);
    TEST_CHECK(fileKind("readme.txt").language == Language::Plain);
    // A prefix needs a separator after it.
    TEST_CHECK(fileKind("README-dev").language == Language::Plain);
    TEST_CHECK(fileKind("CHANGELOG.old").language == Language::Plain);
    TEST_CHECK(fileKind("Makefilex").content == ContentKind::Other);
    TEST_CHECK(fileKind("dockerfiles").content == ContentKind::Other);
    // Case of the whole name does not matter.
    TEST_CHECK(fileKind("TREE.CPP").language == Language::Cpp);
    TEST_CHECK(fileKind("MAKEFILE").language == Language::Make);
  }

  void testDegenerateNames() {
    for (const auto* name : {"", ".", "..", "a.", "file.", ".hidden", "...", "a..", "dir/"}) {
      TEST_CHECK((fileKind(name) == FileKind{}));
    }
    // A leading dot alone is not an extension; the rest is the name.
    TEST_CHECK(fileKind(".json").content == ContentKind::Other);
    TEST_CHECK(fileKind("x.json").content == ContentKind::Text);
    // Bytes outside ASCII never match and never break the lookup.
    TEST_CHECK(fileKind("\xC3\xA9t\xC3\xA9.c").language == Language::C);
    TEST_CHECK(fileKind("\xFF\xFE.\xFF").content == ContentKind::Other);
    const std::string longName(100000, 'a');
    TEST_CHECK(fileKind(longName).content == ContentKind::Other);
    TEST_CHECK(fileKind(longName + ".rs").language == Language::Rust);
  }

  void testProseKinds() {
    // Text meant to be read wraps; text laid out in columns or by machines keeps its lines.
    for (const auto* name :
         {"notes.txt", "NOTES.TXT", "a.text", "README", "readme.txt", "LICENSE", "LICENSE-MIT", "COPYING", "AUTHORS",
          "CHANGELOG", "NOTICE", "CONTRIBUTORS", "TODO", "VERSION"}) {
      TEST_CHECK(fileKind(name).prose);
      TEST_CHECK(fileKind(name).language == Language::Plain);
    }
    for (const auto* name :
         {"build.log", "a.rst", "art.nfo", "a.tex", "yarn.lock", ".gitignore", ".dockerignore", ".gitattributes",
          ".gitmodules", "main.c", "a.json", "a.md", "blob.bin", "Makefile"}) {
      TEST_CHECK(!fileKind(name).prose);
    }
    TEST_CHECK(!FileKind{}.prose);
    TEST_CHECK((fileKind("a.txt") != fileKind("a.log")));
  }

  void testTextSyntax() {
    TEST_CHECK(textSyntax(Language::None) == TextSyntax::None);
    TEST_CHECK(textSyntax(Language::Plain) == TextSyntax::Plain);
    TEST_CHECK(textSyntax(Language::Markdown) == TextSyntax::Markdown);
    TEST_CHECK(textSyntax(Language::Json) == TextSyntax::Json);
    TEST_CHECK(textSyntax(Language::JsonLines) == TextSyntax::JsonLines);
    TEST_CHECK(textSyntax(Language::Yaml) == TextSyntax::Yaml);
    TEST_CHECK(textSyntax(Language::Toml) == TextSyntax::Toml);
    TEST_CHECK(textSyntax(Language::Ini) == TextSyntax::Ini);
    TEST_CHECK(textSyntax(Language::DesktopEntry) == TextSyntax::Ini);
    TEST_CHECK(textSyntax(Language::Xml) == TextSyntax::Xml);
    TEST_CHECK(textSyntax(Language::Html) == TextSyntax::Xml);
    TEST_CHECK(textSyntax(Language::Csv) == TextSyntax::Delimited);
    TEST_CHECK(textSyntax(Language::Tsv) == TextSyntax::Delimited);
    TEST_CHECK(textSyntax(Language::Diff) == TextSyntax::Diff);
    for (const auto language :
         {Language::Code,   Language::C,          Language::Cpp,        Language::CSharp, Language::Java,
          Language::Kotlin, Language::EcmaScript, Language::TypeScript, Language::Rust,   Language::Go,
          Language::Swift,  Language::Zig,        Language::Php,        Language::Python, Language::Ruby,
          Language::Lua,    Language::Shell,      Language::Fish,       Language::Nix,    Language::Haskell,
          Language::Sql,    Language::Meson,      Language::CMake,      Language::Make,   Language::Dockerfile,
          Language::Css,    Language::Scss}) {
      TEST_CHECK(textSyntax(language) == TextSyntax::Code);
    }
    TEST_CHECK(textSyntax(static_cast<Language>(200)) == TextSyntax::None);
  }
} // namespace

int main() {
  testExtensionsCaseNamesAndUnknown();
  testCommonNames();
  testDeveloperLanguages();
  testLookupOrder();
  testDegenerateNames();
  testProseKinds();
  testTextSyntax();
  std::println("formats file kind checks passed");
  return 0;
}
