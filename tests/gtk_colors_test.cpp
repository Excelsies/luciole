#include "render/core/color.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/gtk_colors.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::GtkColors;
  using luciole::readGtkColors;
  using luciole::readGtkFile;

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    TEST_CHECK(file.good());
  }

  GtkColors readRoot(const luciole::test::TempFolder& folder, std::string_view css) {
    write(folder.path() / "gtk.css", css);
    return readGtkColors(folder.path() / "gtk.css", readGtkFile);
  }

  bool is(const GtkColors& read, std::string_view name, std::string_view color) {
    const auto found = read.colors.find(name);
    return found != read.colors.end() && found->second == hex(color);
  }

  bool has(const GtkColors& read, std::string_view name) { return read.colors.find(name) != read.colors.end(); }

  void testDefinitionForms() {
    const luciole::test::TempFolder folder("gtk-colors-forms");
    const auto read = readRoot(
        folder,
        "/* @define-color hidden #111111; */\n"
        "@define-color accent_bg_color #123456;\n"
        "@define-color window_fg_color   #abc  ;\n"
        ":root {\n  --window-bg-color: #11223344;\n  --extra:#ffffff\n}\n"
        "label { color: var(--window-bg-color); }\n"
    );
    TEST_CHECK(is(read, "accent_bg_color", "#123456"));
    TEST_CHECK(is(read, "window_fg_color", "#aabbcc"));
    TEST_CHECK(is(read, "window_bg_color", "#11223344")); // a `--name` is the same name as `name`
    TEST_CHECK(!has(read, "hidden"));                     // inside a comment
    TEST_CHECK(is(read, "extra", "#ffffff"));             // a value may end at the block's closing brace
    TEST_CHECK(read.colors.size() == 4);
    TEST_CHECK(read.files.size() == 1 && read.files[0] == folder.path() / "gtk.css");
  }

  void testLaterDefinitionWins() {
    const luciole::test::TempFolder folder("gtk-colors-later");
    const auto read = readRoot(
        folder,
        "@define-color a #111111;\n:root { --a: #222222; }\n@define-color a #333333;\n"
        "@define-color b #444444;\n:root { --b: oklab(from var(--a) l a b); }\n"
    );
    TEST_CHECK(is(read, "a", "#333333"));
    TEST_CHECK(!has(read, "b")); // the later value is not understood, so the name has none
  }

  void testReferences() {
    const luciole::test::TempFolder folder("gtk-colors-refs");
    const auto read = readRoot(
        folder,
        "@define-color base #102030;\n"
        "@define-color viaAt @base;\n"
        "@define-color late @defined_later;\n"
        ":root { --via-var: var(--base); --chain: @viaAt; --missing: @nothing; --bad: var(--nothing); }\n"
        "@define-color defined_later #405060;\n"
    );
    TEST_CHECK(is(read, "viaAt", "#102030"));
    TEST_CHECK(is(read, "via_var", "#102030"));
    TEST_CHECK(is(read, "chain", "#102030"));
    TEST_CHECK(is(read, "late", "#405060")); // references resolve after everything is read
    TEST_CHECK(!has(read, "missing") && !has(read, "bad"));
  }

  std::string chainOf(int references) {
    std::string css = "@define-color c0 #0a0b0c;\n";
    for (int i = 1; i <= references; ++i) {
      css += std::format("@define-color c{} @c{};\n", i, i - 1);
    }
    return css;
  }

  void testReferenceDepthAndCycles() {
    const luciole::test::TempFolder folder("gtk-colors-depth");
    const auto read = readRoot(folder, chainOf(10));
    TEST_CHECK(is(read, "c8", "#0a0b0c")); // eight references deep
    TEST_CHECK(!has(read, "c9"));          // nine is past the bound
    TEST_CHECK(!has(read, "c10"));
    const auto loop = readRoot(folder, "@define-color a @b;\n@define-color b @a;\n@define-color s @s;\n");
    TEST_CHECK(loop.colors.empty());
  }

  void testUnsupportedValues() {
    const luciole::test::TempFolder folder("gtk-colors-unsupported");
    const auto read = readRoot(
        folder,
        "@define-color a mix(#000000, #ffffff, 0.5);\n@define-color b shade(@a, 0.9);\n"
        "@define-color c alpha(#ffffff, 0.5);\n@define-color d rgb(1, 2, 3);\n"
        "@define-color e #12345;\n@define-color f #gggggg;\n@define-color g red;\n"
        ":root { --h: oklab(from var(--a) l a b); --i: 12px; --j: ; --k #ffffff; }\n"
        "@define-color ok #010203;\n"
    );
    TEST_CHECK(read.colors.size() == 1 && is(read, "ok", "#010203"));
  }

  void testMalformedInput() {
    const luciole::test::TempFolder folder("gtk-colors-malformed");
    const std::string binary = std::string("\0\xff\xfe", 3) + "@define-color a #111111;" + std::string(1, '\0');
    TEST_CHECK(readRoot(folder, "@define-color a #111111; /* never closed").colors.size() == 1);
    TEST_CHECK(readRoot(folder, "@define-color a #111111").colors.empty()); // no terminator
    TEST_CHECK(readRoot(folder, "@import url(\"x.css\"").colors.empty());
    TEST_CHECK(readRoot(folder, "@define-color @@ ;;; { { } ) ( \"unterminated").colors.empty());
    TEST_CHECK(readRoot(folder, binary).colors.size() == 1);
    TEST_CHECK(readRoot(folder, "").colors.empty());
  }

  void testImportForms() {
    const luciole::test::TempFolder folder("gtk-colors-imports");
    const auto root = folder.path() / "gtk-4.0";
    write(root / "one.css", "@define-color one #010101;\n");
    write(root / "two.css", "@define-color two #020202;\n");
    write(root / "three.css", "@define-color three #030303;\n");
    write(root / "sub/four.css", "@define-color four #040404;\n@import url(\"../five.css\");\n");
    write(root / "five.css", "@define-color five #050505;\n");
    write(folder.path() / "abs dir/six.css", "@define-color six #060606;\n");
    write(
        root / "gtk.css",
        std::format(
            "@import url(\"one.css\");\n@import url(two.css);\n@import \"three.css\";\n@import 'sub/four.css';\n"
            "@import url(\"file://{}/abs%20dir/six.css\");\n@import url(\"https://example.org/seven.css\");\n"
            "@import url(\"data:text/css,@define-color seven #070707;\");\n",
            folder.path().string()
        )
    );
    const auto read = readGtkColors(root / "gtk.css", readGtkFile);
    for (const auto& [name, color] :
         {std::pair{"one", "#010101"},
          {"two", "#020202"},
          {"three", "#030303"},
          {"four", "#040404"},
          {"five", "#050505"},
          {"six", "#060606"}}) {
      TEST_CHECK(is(read, name, color));
    }
    TEST_CHECK(!has(read, "seven"));
    TEST_CHECK(read.files.size() == 7 && read.files[0] == root / "gtk.css");

    write(root / "abs.css", std::format("@import url({}/one.css);\n", root.string()));
    const auto absolute = readGtkColors(root / "abs.css", readGtkFile);
    TEST_CHECK(is(absolute, "one", "#010101"));
  }

  void testImportOrder() {
    const luciole::test::TempFolder folder("gtk-colors-order");
    write(folder.path() / "theme.css", "@define-color a #aaaaaa;\n@define-color b #bbbbbb;\n");
    const auto read =
        readRoot(folder, "@define-color a #111111;\n@import url(\"theme.css\");\n@define-color b #222222;\n");
    TEST_CHECK(is(read, "a", "#aaaaaa")); // the import is read where it stands
    TEST_CHECK(is(read, "b", "#222222"));
  }

  void testRelativeToImporter() {
    const luciole::test::TempFolder folder("gtk-colors-relative");
    write(folder.path() / "a/gtk.css", "@import \"b/mid.css\";\n");
    write(folder.path() / "a/b/mid.css", "@import \"leaf.css\";\n");
    write(folder.path() / "a/b/leaf.css", "@define-color leaf #0f0f0f;\n");
    write(folder.path() / "a/leaf.css", "@define-color leaf #f0f0f0;\n");
    const auto read = readGtkColors(folder.path() / "a/gtk.css", readGtkFile);
    TEST_CHECK(is(read, "leaf", "#0f0f0f"));
  }

  void testImportDepth() {
    const luciole::test::TempFolder folder("gtk-colors-import-depth");
    for (int i = 0; i <= 6; ++i) {
      write(
          folder.path() / std::format("f{}.css", i),
          std::format("@define-color d{} #0{}0000;\n@import url(\"f{}.css\");\n", i, i, i + 1)
      );
    }
    const auto read = readGtkColors(folder.path() / "f0.css", readGtkFile);
    TEST_CHECK(has(read, "d4"));  // four imports below the root
    TEST_CHECK(!has(read, "d5")); // the fifth is not followed
    TEST_CHECK(read.files.size() == 5);
  }

  void testFileCountAndCycles() {
    const luciole::test::TempFolder folder("gtk-colors-count");
    std::string root;
    for (int i = 0; i < 40; ++i) {
      write(folder.path() / std::format("m{}.css", i), std::format("@define-color m{} #0a0a0a;\n", i));
      root += std::format("@import url(\"m{}.css\");\n", i);
    }
    const auto read = readRoot(folder, root);
    TEST_CHECK(read.files.size() == luciole::kGtkMaxFiles);
    TEST_CHECK(has(read, "m14") && !has(read, "m15")); // the root and fifteen imports

    write(folder.path() / "x.css", "@import \"y.css\";\n@import \"x.css\";\n@define-color x #111111;\n");
    write(folder.path() / "y.css", "@import \"x.css\";\n@import \"y.css\";\n@define-color y #222222;\n");
    const auto loop = readGtkColors(folder.path() / "x.css", readGtkFile);
    TEST_CHECK(loop.files.size() == 2 && is(loop, "x", "#111111") && is(loop, "y", "#222222"));

    write(folder.path() / "twice.css", "@import \"m0.css\";\n@import \"m0.css\";\n@import \"./m0.css\";\n");
    TEST_CHECK(readGtkColors(folder.path() / "twice.css", readGtkFile).files.size() == 2);
  }

  void testFileSizeAndKind() {
    const luciole::test::TempFolder folder("gtk-colors-kind");
    const std::string filler(luciole::kGtkMaxFileBytes - 64, ' ');
    write(folder.path() / "exact.css", filler + "@define-color fits #010203;\n");
    TEST_CHECK(fs::file_size(folder.path() / "exact.css") <= luciole::kGtkMaxFileBytes);
    TEST_CHECK(is(readGtkColors(folder.path() / "exact.css", readGtkFile), "fits", "#010203"));
    write(folder.path() / "big.css", filler + std::string(128, ' ') + "@define-color big #010203;\n");
    TEST_CHECK(fs::file_size(folder.path() / "big.css") > luciole::kGtkMaxFileBytes);
    const auto big = readGtkColors(folder.path() / "big.css", readGtkFile);
    TEST_CHECK(big.colors.empty() && big.files.empty());

    fs::create_directories(folder.path() / "dir.css");
    write(folder.path() / "root.css", "@import \"dir.css\";\n@import \"link.css\";\n@define-color r #0b0b0b;\n");
    write(folder.path() / "target.css", "@define-color linked #0c0c0c;\n");
    fs::create_symlink(folder.path() / "target.css", folder.path() / "link.css");
    TEST_CHECK(::mkfifo((folder.path() / "pipe.css").c_str(), 0600) == 0);
    write(folder.path() / "root2.css", "@import \"pipe.css\";\n@define-color r2 #0d0d0d;\n");
    const auto read = readGtkColors(folder.path() / "root.css", readGtkFile);
    TEST_CHECK(is(read, "linked", "#0c0c0c") && is(read, "r", "#0b0b0b")); // a link to a regular file is read
    TEST_CHECK(read.files.size() == 2);                                    // the directory is not
    TEST_CHECK(is(readGtkColors(folder.path() / "root2.css", readGtkFile), "r2", "#0d0d0d")); // a pipe is not opened
    TEST_CHECK(!readGtkFile(folder.path() / "pipe.css") && !readGtkFile(folder.path() / "missing.css"));
  }

  void testMissingRoot() {
    const luciole::test::TempFolder folder("gtk-colors-missing");
    const auto read = readGtkColors(folder.path() / "gtk.css", readGtkFile);
    TEST_CHECK(read.colors.empty() && read.files.empty());
  }

  void testCssPath() {
    const luciole::test::TempFolder folder("gtk-colors-path");
    TEST_CHECK(::setenv("XDG_CONFIG_HOME", folder.path().c_str(), 1) == 0);
    TEST_CHECK(luciole::gtkCssPath() == folder.path() / "gtk-4.0" / "gtk.css");
  }
} // namespace

int main() {
  testDefinitionForms();
  testLaterDefinitionWins();
  testReferences();
  testReferenceDepthAndCycles();
  testUnsupportedValues();
  testMalformedInput();
  testImportForms();
  testImportOrder();
  testRelativeToImporter();
  testImportDepth();
  testFileCountAndCycles();
  testFileSizeAndKind();
  testMissingRoot();
  testCssPath();
  std::println("gtk colors checks passed");
  return 0;
}
