#include "temp_folder.h"
#include "test_check.h"
#include "ui/palette_file.h"
#include "ui/palette_source.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string_view>

namespace {
  namespace fs = std::filesystem;
  using luciole::choosePalette;
  using luciole::ColorScheme;
  using luciole::PaletteSource;

  constexpr std::string_view kUsableCss = "@define-color window_bg_color #112233;\n"
                                          "@define-color window_fg_color #eeeeee;\n"
                                          "@define-color accent_bg_color #ff8800;\n";
  constexpr std::string_view kUnusableCss = "@define-color window_bg_color #112233;\n";
  constexpr std::string_view kColors = "[colors]\nsurface = \"#010101\"\n[dark]\nsurface = \"#020202\"\n";

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    TEST_CHECK(file.good());
  }

  void testDesktopColorsWithoutColorFile() {
    const luciole::test::TempFolder folder("palette-source-desktop");
    write(folder.path() / "gtk.css", kUsableCss);
    const auto chosen = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto);
    TEST_CHECK(chosen.has_value());
    TEST_CHECK(chosen->source == PaletteSource::DesktopColors && chosen->readsDesktopColors);
    TEST_CHECK(chosen->palette.surface == hex("#112233") && chosen->palette.primary == hex("#ff8800"));
    TEST_CHECK(chosen->gtkFiles.size() == 1 && chosen->gtkFiles[0] == folder.path() / "gtk.css");
  }

  void testBuiltInWhenDesktopColorsAreUnusable() {
    const luciole::test::TempFolder folder("palette-source-builtin");
    const auto builtIn = luciole::defaultPalette(ColorScheme::Auto);
    const auto none = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto);
    TEST_CHECK(none && none->source == PaletteSource::BuiltIn && none->palette == builtIn);
    TEST_CHECK(none->readsDesktopColors && none->gtkFiles.empty());
    write(folder.path() / "gtk.css", kUnusableCss);
    const auto partial = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto);
    TEST_CHECK(partial && partial->source == PaletteSource::BuiltIn && partial->palette == builtIn);
    TEST_CHECK(partial->gtkFiles.size() == 1);
  }

  void testColorFileWins() {
    const luciole::test::TempFolder folder("palette-source-file");
    write(folder.path() / "gtk.css", kUsableCss);
    write(folder.path() / "colors.toml", kColors);
    const auto chosen = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto);
    TEST_CHECK(chosen && chosen->source == PaletteSource::ColorFile && !chosen->readsDesktopColors);
    TEST_CHECK(chosen->palette.surface == hex("#010101") && chosen->gtkFiles.empty());
  }

  void testInvalidColorFileIsAnError() {
    const luciole::test::TempFolder folder("palette-source-invalid");
    write(folder.path() / "gtk.css", kUsableCss);
    write(folder.path() / "colors.toml", "[colors]\nsurface = \"blue\"\n");
    TEST_CHECK(!choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto));
    write(folder.path() / "colors.toml", "[dark]\nsurface = \"#000000\"\n");
    TEST_CHECK(!choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Auto));
  }

  void testExplicitSchemesIgnoreDesktopColors() {
    const luciole::test::TempFolder folder("palette-source-explicit");
    write(folder.path() / "gtk.css", kUsableCss);
    for (const auto scheme : {ColorScheme::Dark, ColorScheme::Light}) {
      const auto bare = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", scheme);
      TEST_CHECK(bare && bare->source == PaletteSource::BuiltIn && bare->palette == luciole::defaultPalette(scheme));
      TEST_CHECK(!bare->readsDesktopColors && bare->gtkFiles.empty());
    }
    write(folder.path() / "colors.toml", kColors);
    const auto dark = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Dark);
    TEST_CHECK(dark && dark->source == PaletteSource::ColorFile && dark->palette.surface == hex("#020202"));
    const auto light = choosePalette(folder.path() / "colors.toml", folder.path() / "gtk.css", ColorScheme::Light);
    TEST_CHECK(light && light->palette == luciole::defaultPalette(ColorScheme::Light));
  }

  void testNames() {
    TEST_CHECK(luciole::paletteSourceName(PaletteSource::ColorFile) == "colors.toml");
    TEST_CHECK(luciole::paletteSourceName(PaletteSource::DesktopColors) == "desktop GTK colors");
    TEST_CHECK(luciole::paletteSourceName(PaletteSource::BuiltIn) == "built-in palette");
  }
} // namespace

int main() {
  testDesktopColorsWithoutColorFile();
  testBuiltInWhenDesktopColorsAreUnusable();
  testColorFileWins();
  testInvalidColorFileIsAnError();
  testExplicitSchemesIgnoreDesktopColors();
  testNames();
  std::println("palette source checks passed");
  return 0;
}
