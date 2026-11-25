#include "ui/desktop_palette.h"

#include "ui/color_mix.h"
#include "ui/palette_file.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace {
  using luciole::contrastRatio;
  using luciole::GtkColorTable;
  using luciole::mixOklab;

  constexpr float kTextRatio = 4.5F;
  constexpr float kAccentRatio = 3.0F;
  constexpr float kVariantRatio = 3.0F;
  constexpr float kOutlineRatio = 1.25F;
  constexpr float kContainerStep = 0.07F;
  constexpr float kHoverStep = 0.12F;
  constexpr float kOutlineStart = 0.16F;
  constexpr float kOutlineStride = 0.02F;
  constexpr float kVariantStart = 0.55F;
  constexpr float kVariantStride = 0.05F;
  constexpr float kAccentContainerMix = 0.30F;
  constexpr float kSecondaryTurns = 30.0F / 360.0F;
  constexpr float kSecondaryChroma = 0.85F;
  constexpr float kTertiaryTurns = 60.0F / 360.0F;
  constexpr float kTertiaryChroma = 0.9F;
  constexpr float kSurfaceFarthest = 2.0F;
  constexpr float kFlatSurface = 0.01F;
  constexpr int kBisections = 14;
  constexpr int kStrideSteps = 48;

  struct Base {
    Color surface;
    Color onSurface;
    bool dark;
  };

  std::optional<Color> find(const GtkColorTable& names, std::string_view name) {
    const auto found = names.find(name);
    return found == names.end() ? std::nullopt : std::optional<Color>(found->second);
  }

  std::optional<Color> firstOf(const GtkColorTable& names, std::initializer_list<std::string_view> list) {
    for (const auto name : list) {
      if (const auto color = find(names, name)) {
        return color;
      }
    }
    return std::nullopt;
  }

  // Mixes `from` toward `toward` in twentieths until it reads against `background` at the ratio, else `toward`.
  Color reach(const Color& from, const Color& background, const Color& toward, float ratio) {
    constexpr int kSteps = 20;
    for (int step = 0; step < kSteps; ++step) {
      const Color candidate = step == 0 ? from : mixOklab(from, toward, static_cast<float>(step) / kSteps);
      if (contrastRatio(candidate, background) >= ratio) {
        return candidate;
      }
    }
    return toward;
  }

  // Text on a derived fill: the theme's text or surface color, else black or white, whichever reads best.
  Color textOn(const Color& fill, const Base& base) {
    const Color best =
        contrastRatio(base.onSurface, fill) >= contrastRatio(base.surface, fill) ? base.onSurface : base.surface;
    return contrastRatio(best, fill) >= kTextRatio ? best : readableTextColorForBackground(fill);
  }

  // The color nearest `preferred` toward the text color that reads on `fill`, else black or white.
  Color legibleOn(const Color& preferred, const Color& fill, const Base& base) {
    const Color text = reach(preferred, fill, base.onSurface, kTextRatio);
    return contrastRatio(text, fill) >= kTextRatio ? text : readableTextColorForBackground(fill);
  }

  struct Accent {
    Color fill;
    Color onFill;
    Color container;
    Color onContainer;
  };

  Accent accentOf(const Color& fill, const std::optional<Color>& onFill, const Base& base) {
    Accent accent{.fill = fill, .onFill = onFill ? *onFill : textOn(fill, base), .container = {}, .onContainer = {}};
    accent.container = mixOklab(base.surface, fill, kAccentContainerMix);
    accent.onContainer = legibleOn(fill, accent.container, base);
    return accent;
  }

  struct Surfaces {
    Color low;
    Color base;
    Color high;
    Color highest;
  };

  // The containers step away from the surface along the way to `given` (or to a seventh of the way to the text
  // color); the farther ones stop where the text would no longer read.
  Surfaces surfacesOf(const std::optional<Color>& given, const Base& base) {
    const Color& surface = base.surface;
    const Color virtualStep = mixOklab(surface, base.onSurface, kContainerStep);
    const bool flat = given && luciole::oklabDistance(surface, *given) < kFlatSurface;
    const Color unit = given && !flat ? *given : virtualStep;
    const auto at = [&](float k) { return mixOklab(surface, unit, k); };
    const auto reads = [&](float k) { return contrastRatio(at(k), base.onSurface) >= kTextRatio; };
    float limit = kSurfaceFarthest;
    if (!reads(limit)) {
      float near = 0.0F;
      float far = limit;
      for (int step = 0; step < kBisections; ++step) {
        const float mid = (near + far) / 2.0F;
        (reads(mid) ? near : far) = mid;
      }
      limit = near;
    }
    const Color centre = given ? *given : virtualStep;
    const float floor = flat ? 0.0F : 1.0F;
    const float high = std::max(floor, std::min(1.5F, limit));
    const float highest = std::max(high, std::min(kSurfaceFarthest, limit));
    return {.low = flat ? centre : at(0.5F), .base = centre, .high = at(high), .highest = at(highest)};
  }

  // Secondary text: from the surface toward the text color, from 55% on, until it reads at 3:1 on the surface and on
  // every fill it is drawn on: the highest container and the selected-row fill. When no step does, black or white,
  // whichever reads better on the fill it reads worst on.
  Color variantText(const Base& base, const Color& highest, const Color& selected) {
    const std::array fills{base.surface, highest, selected};
    const auto worst = [&fills](const Color& text) {
      float least = std::numeric_limits<float>::max();
      for (const auto& fill : fills) {
        least = std::min(least, contrastRatio(text, fill));
      }
      return least;
    };
    for (int step = 0; step < kStrideSteps; ++step) {
      const float t = std::min(1.0F, kVariantStart + (kVariantStride * static_cast<float>(step)));
      const Color candidate = mixOklab(base.surface, base.onSurface, t);
      if (worst(candidate) >= kVariantRatio) {
        return candidate;
      }
    }
    const Color black = rgba(0.0F, 0.0F, 0.0F);
    const Color white = rgba(1.0F, 1.0F, 1.0F);
    return worst(white) > worst(black) ? white : black;
  }

  // A quiet line: from 16% of the way to the text color, in steps, until it shows on the surface and the container.
  Color outlineOf(const Base& base, const Color& container) {
    for (int step = 0; step < kStrideSteps; ++step) {
      const float t = std::min(1.0F, kOutlineStart + (kOutlineStride * static_cast<float>(step)));
      const Color candidate = mixOklab(base.surface, base.onSurface, t);
      if (contrastRatio(candidate, base.surface) >= kOutlineRatio
          && contrastRatio(candidate, container) >= kOutlineRatio) {
        return candidate;
      }
    }
    return readableTextColorForBackground(base.surface);
  }

  Color accentHue(const Color& primary, float turns, float chroma, const Base& base) {
    const Color color = reach(luciole::rotateHue(primary, turns, chroma), base.surface, base.onSurface, kAccentRatio);
    return contrastRatio(color, base.surface) >= kAccentRatio ? color : readableTextColorForBackground(base.surface);
  }
} // namespace

std::optional<Palette> luciole::desktopPalette(const GtkColorTable& names) {
  const auto surface = find(names, "window_bg_color");
  const auto onSurface = find(names, "window_fg_color");
  const auto primary = find(names, "accent_bg_color");
  // A translucent window color is not a surface the derivations can reason about.
  if (!surface || !onSurface || !primary || surface->a < 1.0F || onSurface->a < 1.0F) {
    return std::nullopt;
  }
  const Base base{
      .surface = *surface, .onSurface = *onSurface, .dark = relativeLuminance(*surface) < relativeLuminance(*onSurface)
  };
  const Palette builtIn = defaultPalette(base.dark ? ColorScheme::Dark : ColorScheme::Light);

  const auto errorFill = firstOf(names, {"error_bg_color", "destructive_bg_color"});
  const auto errorText = firstOf(names, {"error_fg_color", "destructive_fg_color"});
  const Accent primaryAccent = accentOf(*primary, find(names, "accent_fg_color"), base);
  const Accent secondary = accentOf(accentHue(*primary, kSecondaryTurns, kSecondaryChroma, base), std::nullopt, base);
  const Accent tertiary = accentOf(accentHue(*primary, kTertiaryTurns, kTertiaryChroma, base), std::nullopt, base);
  const Accent error =
      errorFill ? accentOf(*errorFill, errorText, base) : accentOf(builtIn.error, builtIn.onError, base);

  const auto container =
      firstOf(names, {"card_bg_color", "popover_bg_color", "sidebar_bg_color", "headerbar_bg_color"});
  const Surfaces surfaces = surfacesOf(container, base);

  auto outline = firstOf(names, {"border_color"});
  if (const auto sidebar = find(names, "sidebar_border_color"); !outline && sidebar && *sidebar != base.surface) {
    outline = sidebar;
  }
  const Color hover =
      find(names, "active_toggle_bg_color").value_or(mixOklab(base.surface, base.onSurface, kHoverStep));

  return Palette{
      .primary = primaryAccent.fill,
      .onPrimary = primaryAccent.onFill,
      .primaryContainer = primaryAccent.container,
      .onPrimaryContainer = primaryAccent.onContainer,
      .secondary = secondary.fill,
      .onSecondary = secondary.onFill,
      .secondaryContainer = secondary.container,
      .onSecondaryContainer = secondary.onContainer,
      .tertiary = tertiary.fill,
      .onTertiary = tertiary.onFill,
      .tertiaryContainer = tertiary.container,
      .onTertiaryContainer = tertiary.onContainer,
      .error = error.fill,
      .onError = error.onFill,
      .errorContainer = error.container,
      .onErrorContainer = error.onContainer,
      .surface = base.surface,
      .onSurface = base.onSurface,
      .onSurfaceVariant = variantText(base, surfaces.highest, primaryAccent.container),
      .surfaceContainerLow = surfaces.low,
      .surfaceContainer = surfaces.base,
      .surfaceContainerHigh = surfaces.high,
      .surfaceContainerHighest = surfaces.highest,
      .outlineVariant = outline.value_or(outlineOf(base, surfaces.base)),
      .shadow = base.dark ? mixOklab(base.surface, rgba(0.0F, 0.0F, 0.0F), 0.5F) : base.onSurface,
      .scrim = rgba(0.0F, 0.0F, 0.0F),
      .hover = hover,
      .onHover = find(names, "active_toggle_fg_color").value_or(textOn(hover, base)),
  };
}
