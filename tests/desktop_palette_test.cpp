#include "render/core/color.h"
#include "temp_folder.h"
#include "test_check.h"
#include "ui/desktop_palette.h"
#include "ui/palette_file.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using luciole::GtkColors;
  using luciole::GtkColorTable;

  void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    TEST_CHECK(file.good());
  }

  // WCAG 2 relative luminance and contrast, written out here rather than taken from the code under test. The
  // library linearizes with a 0.03928 threshold, the standard's current text says 0.04045.
  double linear(float channel) {
    const double c = channel;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  }

  double luminance(const Color& color) {
    return (0.2126 * linear(color.r)) + (0.7152 * linear(color.g)) + (0.0722 * linear(color.b));
  }

  double ratio(const Color& a, const Color& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
  }

  struct Fixture {
    std::string name;
    Palette palette;
    bool dark;
  };

  // The structure of Noctalia's GTK 4 template: names, then the same colors in :root, some as references.
  std::string noctaliaCss(
      std::string_view surface, std::string_view onSurface, std::string_view primary, std::string_view onPrimary,
      std::string_view container, std::string_view error, std::string_view onError
  ) {
    return std::format(
        R"(/* GTK Colors (GTK4) */
@define-color accent_color {1};
@define-color accent_bg_color {1};
@define-color accent_fg_color {2};
@define-color destructive_bg_color {5};
@define-color destructive_fg_color {6};
@define-color error_bg_color {5};
@define-color error_fg_color {6};
@define-color window_bg_color {3};
@define-color window_fg_color {4};
@define-color view_bg_color {3};
@define-color view_fg_color {4};
@define-color headerbar_bg_color {3};
@define-color headerbar_fg_color {4};
@define-color headerbar_backdrop_color @window_bg_color;
@define-color popover_bg_color {7};
@define-color popover_fg_color {4};
@define-color card_bg_color {7};
@define-color card_fg_color {4};
@define-color dialog_bg_color {3};
@define-color overview_bg_color {7};
@define-color sidebar_bg_color {7};
@define-color sidebar_fg_color {4};
@define-color sidebar_backdrop_color @window_bg_color;
@define-color sidebar_border_color @window_bg_color;
@define-color secondary_sidebar_bg_color {3};
@define-color theme_selected_bg_color @accent_bg_color;
:root {{
    --accent-color: {1};
    --accent-bg-color: {1};
    --accent-fg-color: {2};
    --destructive-bg-color: {5};
    --window-bg-color: {3};
    --window-fg-color: {4};
    --card-bg-color: {7};
    --headerbar-backdrop-color: @window_bg_color;
    --sidebar-border-color: @window_bg_color;
}}
)",
        "", primary, onPrimary, surface, onSurface, error, onError, container
    );
  }

  // The structure of Chromancy Theming: a gtk.css that imports a stylesheet by an encoded absolute file URL.
  void writeChromancy(const fs::path& config) {
    const auto sheet = config / "my theme" / "chromancy-theming.css";
    write(
        sheet,
        R"(/* Chromancy Theming: palette "Jeweltone Dark". Rewritten on every Apply. */

@define-color accent_bg_color #e3c078;
@define-color accent_fg_color #17120a;
@define-color accent_color #e3c078;
@define-color window_bg_color #0b0910;
@define-color window_fg_color #ede4d9;
@define-color view_bg_color #0b0910;
@define-color view_fg_color #ede4d9;
@define-color headerbar_bg_color #191420;
@define-color headerbar_fg_color #ede4d9;
@define-color sidebar_bg_color #191420;
@define-color secondary_sidebar_bg_color #0b0910;
@define-color card_bg_color #191420;
@define-color dialog_bg_color #191420;
@define-color popover_bg_color #191420;
@define-color thumbnail_bg_color #191420;
@define-color overview_bg_color #191420;
@define-color active_toggle_bg_color #241c2e;
@define-color active_toggle_fg_color #f3eae0;
@define-color destructive_bg_color #e8736b;
@define-color destructive_fg_color #17120a;
@define-color success_bg_color #43e597;
@define-color warning_bg_color #e3c078;
@define-color error_bg_color #e8736b;
@define-color error_fg_color #17120a;

:root {
  --accent-bg-color: #e3c078;
  --accent-fg-color: #17120a;
  --accent-color: oklab(from var(--accent-bg-color) var(--standalone-color-oklab));
  --yaru-accent-bg-color: #e3c078;
  --window-bg-color: #0b0910;
  --window-fg-color: #ede4d9;
  --headerbar-bg-color: #191420;
  --active-toggle-bg-color: #241c2e;
  --active-toggle-fg-color: #f3eae0;
  --headerbar-backdrop-color: #0b0910;
  --sidebar-border-color: #6b5c77;
  --secondary-sidebar-border-color: #6b5c77;
  --border-color: #6b5c77;
  --card-shade-color: #6b5c7799;
  --error-bg-color: #e8736b;
}
)"
    );
    write(
        config / "gtk.css",
        std::format(
            "/* user rules */\n@import url(\"file://{}/my%20theme/chromancy-theming.css\"); /* chromancy */\n",
            config.string()
        )
    );
  }

  GtkColorTable table(const fs::path& config) { return luciole::readGtkColors(config / "gtk.css").colors; }

  Palette derive(const GtkColorTable& names) {
    const auto result = luciole::desktopPalette(names);
    TEST_CHECK(result.has_value());
    return *result;
  }

  Palette fromCss(const std::string& css) {
    const luciole::test::TempFolder folder("desktop-palette-css");
    write(folder.path() / "gtk.css", css);
    return derive(table(folder.path()));
  }

  std::vector<Fixture> fixtures() {
    std::vector<Fixture> all;
    all.push_back(
        {"noctalia dark",
         fromCss(noctaliaCss("#15162e", "#ece9f4", "#f7e9a8", "#302c1e", "#20213d", "#ed8296", "#3b2029")), true}
    );
    all.push_back(
        {"noctalia light",
         fromCss(noctaliaCss("#dcddea", "#292b45", "#5058b1", "#f0eff7", "#cfd2e4", "#a53c59", "#f0eff7")), false}
    );
    {
      const luciole::test::TempFolder folder("desktop-palette-chromancy");
      writeChromancy(folder.path());
      all.push_back({"chromancy dark", derive(table(folder.path())), true});
    }
    all.push_back(
        {"minimal dark",
         fromCss(
             "@define-color window_bg_color #1e1e2e;\n@define-color window_fg_color #cdd6f4;\n"
             "@define-color accent_bg_color #89b4fa;\n"
         ),
         true}
    );
    all.push_back(
        {"minimal light",
         fromCss(
             ":root { --window-bg-color: #eff1f5; --window-fg-color: #4c4f69; "
             "--accent-bg-color: #1e66f5; }\n"
         ),
         false}
    );
    return all;
  }

  struct Pair {
    const char* name;
    Color text;
    Color fill;
    double minimum;
  };

  std::vector<Pair> floors(const Palette& p) {
    return {
        {"on_primary/primary", p.onPrimary, p.primary, 4.5},
        {"on_secondary/secondary", p.onSecondary, p.secondary, 4.5},
        {"on_tertiary/tertiary", p.onTertiary, p.tertiary, 4.5},
        {"on_error/error", p.onError, p.error, 4.5},
        {"on_hover/hover", p.onHover, p.hover, 4.5},
        {"on_primary_container/primary_container", p.onPrimaryContainer, p.primaryContainer, 4.5},
        {"on_secondary_container/secondary_container", p.onSecondaryContainer, p.secondaryContainer, 4.5},
        {"on_tertiary_container/tertiary_container", p.onTertiaryContainer, p.tertiaryContainer, 4.5},
        {"on_error_container/error_container", p.onErrorContainer, p.errorContainer, 4.5},
        {"on_surface/surface", p.onSurface, p.surface, 4.5},
        {"on_surface/surface_container_low", p.onSurface, p.surfaceContainerLow, 4.5},
        {"on_surface/surface_container", p.onSurface, p.surfaceContainer, 4.5},
        {"on_surface/surface_container_high", p.onSurface, p.surfaceContainerHigh, 4.5},
        {"on_surface/surface_container_highest", p.onSurface, p.surfaceContainerHighest, 4.5},
        {"on_surface_variant/surface", p.onSurfaceVariant, p.surface, 3.0},
        {"on_surface_variant/surface_container", p.onSurfaceVariant, p.surfaceContainer, 3.0},
        {"on_surface_variant/surface_container_highest", p.onSurfaceVariant, p.surfaceContainerHighest, 3.0},
        {"secondary/surface", p.secondary, p.surface, 3.0},
        {"tertiary/surface", p.tertiary, p.surface, 3.0},
        {"outline_variant/surface", p.outlineVariant, p.surface, 1.25},
        {"outline_variant/surface_container", p.outlineVariant, p.surfaceContainer, 1.25},
    };
  }

  void testContrastFloors() {
    for (const auto& fixture : fixtures()) {
      std::println("{}", fixture.name);
      for (const auto& pair : floors(fixture.palette)) {
        const double got = ratio(pair.text, pair.fill);
        std::println("  {:<46} {:6.2f} (min {:.2f})", pair.name, got, pair.minimum);
        if (got < pair.minimum) {
          std::println("  FAILED {} for {}", pair.name, fixture.name);
        }
        TEST_CHECK(got >= pair.minimum);
      }
    }
  }

  double distanceFromSurface(const Palette& p, const Color& color) {
    // Lightness distance is enough to order tints of one surface: they differ mostly in lightness.
    return std::abs(luminance(color) - luminance(p.surface));
  }

  void testContainersAreOrdered() {
    for (const auto& fixture : fixtures()) {
      const auto& p = fixture.palette;
      const double low = distanceFromSurface(p, p.surfaceContainerLow);
      const double base = distanceFromSurface(p, p.surfaceContainer);
      const double high = distanceFromSurface(p, p.surfaceContainerHigh);
      const double highest = distanceFromSurface(p, p.surfaceContainerHighest);
      TEST_CHECK(0.0 < low && low < base && base < high && high < highest);
    }
  }

  void testVariantFollowsTheColors() {
    for (const auto& fixture : fixtures()) {
      const auto& p = fixture.palette;
      TEST_CHECK((luminance(p.surface) < luminance(p.onSurface)) == fixture.dark);
      // A role with nothing to derive from takes the built-in value of the variant found.
      const auto builtIn =
          luciole::defaultPalette(fixture.dark ? luciole::ColorScheme::Dark : luciole::ColorScheme::Light);
      if (fixture.name == "minimal dark" || fixture.name == "minimal light") {
        TEST_CHECK(p.error == builtIn.error);
      }
      TEST_CHECK(p.scrim == hex("#000000"));
    }
  }

  void testRequiredNames() {
    const GtkColorTable full{
        {"window_bg_color", hex("#101010")}, {"window_fg_color", hex("#eeeeee")}, {"accent_bg_color", hex("#3584e4")}
    };
    TEST_CHECK(luciole::desktopPalette(full).has_value());
    for (const auto* missing : {"window_bg_color", "window_fg_color", "accent_bg_color"}) {
      auto partial = full;
      partial.erase(missing);
      TEST_CHECK(!luciole::desktopPalette(partial).has_value());
    }
    TEST_CHECK(!luciole::desktopPalette({}).has_value());
  }

  GtkColorTable base() {
    return {
        {"window_bg_color", hex("#101820")}, {"window_fg_color", hex("#e8eef4")}, {"accent_bg_color", hex("#5599dd")}
    };
  }

  void testExactMapping() {
    auto names = base();
    names.insert({
        {"accent_fg_color", hex("#010203")},
        {"card_bg_color", hex("#1a2230")},
        {"border_color", hex("#445566")},
        {"active_toggle_bg_color", hex("#2a3240")},
        {"active_toggle_fg_color", hex("#fafafa")},
        {"error_bg_color", hex("#d04040")},
        {"error_fg_color", hex("#fff8f8")},
        {"destructive_bg_color", hex("#aa0000")},
        {"destructive_fg_color", hex("#000000")},
    });
    const auto p = derive(names);
    TEST_CHECK(p.primary == hex("#5599dd") && p.onPrimary == hex("#010203"));
    TEST_CHECK(p.surface == hex("#101820") && p.onSurface == hex("#e8eef4"));
    TEST_CHECK(p.surfaceContainer == hex("#1a2230"));
    TEST_CHECK(p.outlineVariant == hex("#445566"));
    TEST_CHECK(p.hover == hex("#2a3240") && p.onHover == hex("#fafafa"));
    TEST_CHECK(p.error == hex("#d04040") && p.onError == hex("#fff8f8")); // error wins over destructive
  }

  void testPreferenceOrder() {
    auto names = base();
    names.insert(
        {{"destructive_bg_color", hex("#aa0000")},
         {"destructive_fg_color", hex("#000000")},
         {"popover_bg_color", hex("#222222")},
         {"sidebar_bg_color", hex("#333333")},
         {"headerbar_bg_color", hex("#444444")}}
    );
    auto p = derive(names);
    TEST_CHECK(p.error == hex("#aa0000") && p.onError == hex("#000000"));
    TEST_CHECK(p.surfaceContainer == hex("#222222"));
    names.erase("popover_bg_color");
    TEST_CHECK(derive(names).surfaceContainer == hex("#333333"));
    names.erase("sidebar_bg_color");
    TEST_CHECK(derive(names).surfaceContainer == hex("#444444"));
    names.erase("headerbar_bg_color");
    const auto derived = derive(names);
    TEST_CHECK(derived.surfaceContainer != derived.surface);
  }

  void testOutlineSources() {
    auto names = base();
    names.insert({{"sidebar_border_color", hex("#101820")}}); // the surface itself: no border
    const auto flat = derive(names);
    TEST_CHECK(flat.outlineVariant != hex("#101820") && ratio(flat.outlineVariant, flat.surface) >= 1.25);
    names["sidebar_border_color"] = hex("#334455");
    TEST_CHECK(derive(names).outlineVariant == hex("#334455"));
    names["border_color"] = hex("#667788");
    TEST_CHECK(derive(names).outlineVariant == hex("#667788"));
  }

  void testThemeColorsAreNeverAltered() {
    auto names = base();
    names.insert(
        {{"accent_fg_color", hex("#5a9ee0")}, // almost the accent: contrast about 1.1
         {"error_bg_color", hex("#202830")},  // near the surface
         {"error_fg_color", hex("#222a32")},
         {"border_color", hex("#111921")}, // nearly invisible
         {"card_bg_color", hex("#111a22")}}
    );
    const auto p = derive(names);
    TEST_CHECK(p.onPrimary == hex("#5a9ee0"));
    TEST_CHECK(p.error == hex("#202830") && p.onError == hex("#222a32"));
    TEST_CHECK(p.outlineVariant == hex("#111921"));
    TEST_CHECK(p.surfaceContainer == hex("#111a22"));
    // What is derived still holds its floors.
    TEST_CHECK(ratio(p.onPrimaryContainer, p.primaryContainer) >= 4.5);
    TEST_CHECK(ratio(p.onErrorContainer, p.errorContainer) >= 4.5);
    TEST_CHECK(ratio(p.onSurfaceVariant, p.surface) >= 3.0);
    TEST_CHECK(ratio(p.onSecondary, p.secondary) >= 4.5 && ratio(p.onTertiary, p.tertiary) >= 4.5);
  }

  void testTranslucentWindowColorsAreNotUsable() {
    auto names = base();
    names["window_bg_color"] = hex("#10182080");
    TEST_CHECK(!luciole::desktopPalette(names).has_value());
    names = base();
    names["window_fg_color"] = hex("#e8eef4cc");
    TEST_CHECK(!luciole::desktopPalette(names).has_value());
    names = base();
    names["accent_bg_color"] = hex("#5599dd80");
    TEST_CHECK(luciole::desktopPalette(names).has_value()); // only the two window colors must be opaque
    names = base();
    names["window_bg_color"] = hex("#101820ff");
    TEST_CHECK(luciole::desktopPalette(names).has_value());
  }

  bool blackOrWhite(const Color& c) { return (c == hex("#000000")) || (c == hex("#ffffff")); }

  // The derived text pairs read at their floors, or are black or white. The theme's own text color is not derived,
  // so its pairs are left out.
  void checkDerivedTextPairs(const Palette& p) {
    const std::vector<Pair> derived{
        {"on_secondary/secondary", p.onSecondary, p.secondary, 4.5},
        {"on_tertiary/tertiary", p.onTertiary, p.tertiary, 4.5},
        {"on_error/error", p.onError, p.error, 4.5},
        {"on_hover/hover", p.onHover, p.hover, 4.5},
        {"on_primary_container", p.onPrimaryContainer, p.primaryContainer, 4.5},
        {"on_secondary_container", p.onSecondaryContainer, p.secondaryContainer, 4.5},
        {"on_tertiary_container", p.onTertiaryContainer, p.tertiaryContainer, 4.5},
        {"on_error_container", p.onErrorContainer, p.errorContainer, 4.5},
        {"on_surface_variant/surface", p.onSurfaceVariant, p.surface, 3.0},
        {"secondary/surface", p.secondary, p.surface, 3.0},
        {"tertiary/surface", p.tertiary, p.surface, 3.0},
        {"outline_variant/surface", p.outlineVariant, p.surface, 1.25},
    };
    for (const auto& pair : derived) {
      const bool holds = ratio(pair.text, pair.fill) >= pair.minimum || blackOrWhite(pair.text);
      if (!holds) {
        std::println("degenerate input: {} is {:.2f}", pair.name, ratio(pair.text, pair.fill));
      }
      TEST_CHECK(holds);
    }
  }

  void testDegenerateColors() {
    for (const auto& [surface, text] :
         {std::pair{"#808080", "#808080"},
          {"#808080", "#818181"},
          {"#000000", "#010101"},
          {"#ffffff", "#fefefe"},
          {"#000000", "#000000"},
          {"#ffffff", "#ffffff"}}) {
      auto names = base();
      names["window_bg_color"] = hex(surface);
      names["window_fg_color"] = hex(text);
      const auto p = derive(names);
      TEST_CHECK(p.surface == hex(surface) && p.onSurface == hex(text)); // the theme's colors stay
      checkDerivedTextPairs(p);
    }
  }

  void testDerivationIsDeterministicAndCoherent() {
    const auto a = derive(base());
    const auto b = derive(base());
    TEST_CHECK(a == b);
    // Derived accents are not the built-in palette's unrelated hues.
    const auto builtIn = luciole::defaultPalette(luciole::ColorScheme::Dark);
    TEST_CHECK(a.secondary != builtIn.secondary && a.tertiary != builtIn.tertiary);
    TEST_CHECK(a.primaryContainer != builtIn.primaryContainer && a.hover != builtIn.hover);
    // The accents are three different colors.
    TEST_CHECK(a.secondary != a.primary && a.tertiary != a.primary && a.tertiary != a.secondary);
    // The blue accent's derived accents stay bluish-green to violet: no red-dominant accent.
    TEST_CHECK(a.secondary.r < std::max(a.secondary.g, a.secondary.b));
  }
} // namespace

int main() {
  testContrastFloors();
  testContainersAreOrdered();
  testVariantFollowsTheColors();
  testRequiredNames();
  testExactMapping();
  testPreferenceOrder();
  testOutlineSources();
  testThemeColorsAreNeverAltered();
  testTranslucentWindowColorsAreNotUsable();
  testDegenerateColors();
  testDerivationIsDeterministicAndCoherent();
  std::println("desktop palette checks passed");
  return 0;
}
