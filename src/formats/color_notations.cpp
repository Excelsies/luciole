#include "formats/color_notations.h"

#include "formats/ascii.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <utility>

namespace luciole::formats {
  namespace {
    constexpr std::size_t kMaxNameBytes = 20; // "lightgoldenrodyellow"

    constexpr Rgba opaque(std::uint32_t rgb) {
      return {
          .r = static_cast<std::uint8_t>(rgb >> 16U),
          .g = static_cast<std::uint8_t>((rgb >> 8U) & 0xFFU),
          .b = static_cast<std::uint8_t>(rgb & 0xFFU),
      };
    }

    struct NamedColor {
      std::string_view name;
      Rgba color;
    };

    constexpr std::array<NamedColor, 149> kNamedColors{{
        {"aliceblue", opaque(0xF0F8FF)},
        {"antiquewhite", opaque(0xFAEBD7)},
        {"aqua", opaque(0x00FFFF)},
        {"aquamarine", opaque(0x7FFFD4)},
        {"azure", opaque(0xF0FFFF)},
        {"beige", opaque(0xF5F5DC)},
        {"bisque", opaque(0xFFE4C4)},
        {"black", opaque(0x000000)},
        {"blanchedalmond", opaque(0xFFEBCD)},
        {"blue", opaque(0x0000FF)},
        {"blueviolet", opaque(0x8A2BE2)},
        {"brown", opaque(0xA52A2A)},
        {"burlywood", opaque(0xDEB887)},
        {"cadetblue", opaque(0x5F9EA0)},
        {"chartreuse", opaque(0x7FFF00)},
        {"chocolate", opaque(0xD2691E)},
        {"coral", opaque(0xFF7F50)},
        {"cornflowerblue", opaque(0x6495ED)},
        {"cornsilk", opaque(0xFFF8DC)},
        {"crimson", opaque(0xDC143C)},
        {"cyan", opaque(0x00FFFF)},
        {"darkblue", opaque(0x00008B)},
        {"darkcyan", opaque(0x008B8B)},
        {"darkgoldenrod", opaque(0xB8860B)},
        {"darkgray", opaque(0xA9A9A9)},
        {"darkgreen", opaque(0x006400)},
        {"darkgrey", opaque(0xA9A9A9)},
        {"darkkhaki", opaque(0xBDB76B)},
        {"darkmag"
         "enta",
         opaque(0x8B008B)},
        {"darkolivegreen", opaque(0x556B2F)},
        {"darkorange", opaque(0xFF8C00)},
        {"darkorchid", opaque(0x9932CC)},
        {"darkred", opaque(0x8B0000)},
        {"darksalmon", opaque(0xE9967A)},
        {"darkseagreen", opaque(0x8FBC8F)},
        {"darkslateblue", opaque(0x483D8B)},
        {"darkslategray", opaque(0x2F4F4F)},
        {"darkslategrey", opaque(0x2F4F4F)},
        {"darkturquoise", opaque(0x00CED1)},
        {"darkviolet", opaque(0x9400D3)},
        {"deeppink", opaque(0xFF1493)},
        {"deepskyblue", opaque(0x00BFFF)},
        {"dimgray", opaque(0x696969)},
        {"dimgrey", opaque(0x696969)},
        {"dodgerblue", opaque(0x1E90FF)},
        {"firebrick", opaque(0xB22222)},
        {"floralwhite", opaque(0xFFFAF0)},
        {"forestgreen", opaque(0x228B22)},
        {"fuchsia", opaque(0xFF00FF)},
        {"gainsboro", opaque(0xDCDCDC)},
        {"ghostwhite", opaque(0xF8F8FF)},
        {"gold", opaque(0xFFD700)},
        {"goldenrod", opaque(0xDAA520)},
        {"gray", opaque(0x808080)},
        {"green", opaque(0x008000)},
        {"greenyellow", opaque(0xADFF2F)},
        {"grey", opaque(0x808080)},
        {"honeydew", opaque(0xF0FFF0)},
        {"hotpink", opaque(0xFF69B4)},
        {"indianred", opaque(0xCD5C5C)},
        {"indigo", opaque(0x4B0082)},
        {"ivory", opaque(0xFFFFF0)},
        {"khaki", opaque(0xF0E68C)},
        {"lavender", opaque(0xE6E6FA)},
        {"lavenderblush", opaque(0xFFF0F5)},
        {"lawngreen", opaque(0x7CFC00)},
        {"lemonchiffon", opaque(0xFFFACD)},
        {"lightblue", opaque(0xADD8E6)},
        {"lightcoral", opaque(0xF08080)},
        {"lightcyan", opaque(0xE0FFFF)},
        {"lightgoldenrodyellow", opaque(0xFAFAD2)},
        {"lightgray", opaque(0xD3D3D3)},
        {"lightgreen", opaque(0x90EE90)},
        {"lightgrey", opaque(0xD3D3D3)},
        {"lightpink", opaque(0xFFB6C1)},
        {"lightsalmon", opaque(0xFFA07A)},
        {"lightseagreen", opaque(0x20B2AA)},
        {"lightskyblue", opaque(0x87CEFA)},
        {"lightslategray", opaque(0x778899)},
        {"lightslategrey", opaque(0x778899)},
        {"lightsteelblue", opaque(0xB0C4DE)},
        {"lightyellow", opaque(0xFFFFE0)},
        {"lime", opaque(0x00FF00)},
        {"limegreen", opaque(0x32CD32)},
        {"linen", opaque(0xFAF0E6)},
        {"mag"
         "enta",
         opaque(0xFF00FF)},
        {"maroon", opaque(0x800000)},
        {"mediumaquamarine", opaque(0x66CDAA)},
        {"mediumblue", opaque(0x0000CD)},
        {"mediumorchid", opaque(0xBA55D3)},
        {"mediumpurple", opaque(0x9370DB)},
        {"mediumseagreen", opaque(0x3CB371)},
        {"mediumslateblue", opaque(0x7B68EE)},
        {"mediumspringgreen", opaque(0x00FA9A)},
        {"mediumturquoise", opaque(0x48D1CC)},
        {"mediumvioletred", opaque(0xC71585)},
        {"midnightblue", opaque(0x191970)},
        {"mintcream", opaque(0xF5FFFA)},
        {"mistyrose", opaque(0xFFE4E1)},
        {"moccasin", opaque(0xFFE4B5)},
        {"navajowhite", opaque(0xFFDEAD)},
        {"navy", opaque(0x000080)},
        {"oldlace", opaque(0xFDF5E6)},
        {"olive", opaque(0x808000)},
        {"olivedrab", opaque(0x6B8E23)},
        {"orange", opaque(0xFFA500)},
        {"orangered", opaque(0xFF4500)},
        {"orchid", opaque(0xDA70D6)},
        {"palegoldenrod", opaque(0xEEE8AA)},
        {"palegreen", opaque(0x98FB98)},
        {"paleturquoise", opaque(0xAFEEEE)},
        {"palevioletred", opaque(0xDB7093)},
        {"papayawhip", opaque(0xFFEFD5)},
        {"peachpuff", opaque(0xFFDAB9)},
        {"peru", opaque(0xCD853F)},
        {"pink", opaque(0xFFC0CB)},
        {"plum", opaque(0xDDA0DD)},
        {"powderblue", opaque(0xB0E0E6)},
        {"purple", opaque(0x800080)},
        {"rebeccapurple", opaque(0x663399)},
        {"red", opaque(0xFF0000)},
        {"rosybrown", opaque(0xBC8F8F)},
        {"royalblue", opaque(0x4169E1)},
        {"saddlebrown", opaque(0x8B4513)},
        {"salmon", opaque(0xFA8072)},
        {"sandybrown", opaque(0xF4A460)},
        {"seagreen", opaque(0x2E8B57)},
        {"seashell", opaque(0xFFF5EE)},
        {"sienna", opaque(0xA0522D)},
        {"silver", opaque(0xC0C0C0)},
        {"skyblue", opaque(0x87CEEB)},
        {"slateblue", opaque(0x6A5ACD)},
        {"slategray", opaque(0x708090)},
        {"slategrey", opaque(0x708090)},
        {"snow", opaque(0xFFFAFA)},
        {"springgreen", opaque(0x00FF7F)},
        {"steelblue", opaque(0x4682B4)},
        {"tan", opaque(0xD2B48C)},
        {"teal", opaque(0x008080)},
        {"thistle", opaque(0xD8BFD8)},
        {"tomato", opaque(0xFF6347)},
        {"transparent", {.r = 0, .g = 0, .b = 0, .a = 0}},
        {"turquoise", opaque(0x40E0D0)},
        {"violet", opaque(0xEE82EE)},
        {"wheat", opaque(0xF5DEB3)},
        {"white", opaque(0xFFFFFF)},
        {"whitesmoke", opaque(0xF5F5F5)},
        {"yellow", opaque(0xFFFF00)},
        {"yellowgreen", opaque(0x9ACD32)},
    }};
    static_assert(std::ranges::is_sorted(kNamedColors, {}, &NamedColor::name));

    enum class Unit : std::uint8_t { None, Percent, Deg, Rad, Grad, Turn };

    struct Component {
      double value = 0;
      Unit unit = Unit::None;
    };

    std::size_t skipDigits(std::string_view s, std::size_t i) {
      while (i < s.size() && isAsciiDigit(s[i])) {
        ++i;
      }
      return i;
    }

    // A CSS number with an optional percent sign or angle unit at i; i moves past it.
    std::optional<Component> readComponent(std::string_view s, std::size_t& i) {
      const auto sign = i < s.size() && (s[i] == '+' || s[i] == '-') ? i + 1 : i;
      auto k = skipDigits(s, sign);
      const bool whole = k > sign;
      if (k < s.size() && s[k] == '.') {
        const auto fraction = skipDigits(s, k + 1);
        if (fraction == k + 1) {
          return std::nullopt;
        }
        k = fraction;
      } else if (!whole) {
        return std::nullopt;
      }
      if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
        const auto digits = k + 1 < s.size() && (s[k + 1] == '+' || s[k + 1] == '-') ? k + 2 : k + 1;
        const auto end = skipDigits(s, digits);
        k = end > digits ? end : k;
      }
      double value = 0;
      const auto* const first = s.data() + (s[i] == '+' ? i + 1 : i);
      const auto [ptr, error] = std::from_chars(first, s.data() + k, value);
      if (error != std::errc{} || ptr != s.data() + k || !std::isfinite(value)) {
        return std::nullopt;
      }
      auto unit = Unit::None;
      if (k < s.size() && s[k] == '%') {
        unit = Unit::Percent;
        ++k;
      } else {
        auto end = k;
        while (end < s.size() && isAsciiAlpha(s[end])) {
          ++end;
        }
        const auto name = s.substr(k, end - k);
        constexpr std::array<std::pair<std::string_view, Unit>, 4> kUnits{
            {{"deg", Unit::Deg}, {"rad", Unit::Rad}, {"grad", Unit::Grad}, {"turn", Unit::Turn}}
        };
        const auto* const known =
            std::ranges::find_if(kUnits, [&](const auto& u) { return equalsIgnoringCase(name, u.first); });
        if (!name.empty() && known == kUnits.end()) {
          return std::nullopt;
        }
        unit = name.empty() ? Unit::None : known->second;
        k = end;
      }
      i = k;
      return Component{.value = value, .unit = unit};
    }

    struct Arguments {
      std::array<Component, 4> parts{};
      std::array<char, 3> separators{}; // ',', '/' or ' ' after each part but the last
      std::size_t count = 0;
    };

    std::optional<Arguments> readArguments(std::string_view args) {
      Arguments out;
      auto i = skipBlanks(args, 0);
      for (std::size_t part = 0; part < out.parts.size(); ++part) {
        const auto component = readComponent(args, i);
        if (!component) {
          return std::nullopt;
        }
        out.parts[part] = *component;
        out.count = part + 1;
        const auto valueEnd = i;
        i = skipBlanks(args, i);
        if (i == args.size()) {
          return out;
        }
        if (part + 1 == out.parts.size()) {
          return std::nullopt;
        }
        if (args[i] == ',' || args[i] == '/') {
          out.separators[part] = args[i];
          i = skipBlanks(args, i + 1);
        } else if (i > valueEnd) {
          out.separators[part] = ' ';
        } else {
          return std::nullopt;
        }
      }
      return std::nullopt;
    }

    std::optional<double> channel(Component c) {
      if (c.unit != Unit::None && c.unit != Unit::Percent) {
        return std::nullopt;
      }
      return std::clamp(c.unit == Unit::Percent ? c.value / 100.0 * 255.0 : c.value, 0.0, 255.0);
    }

    // 0..1 from a number or a percentage.
    std::optional<double> fraction(Component c) {
      if (c.unit != Unit::None && c.unit != Unit::Percent) {
        return std::nullopt;
      }
      return std::clamp(c.unit == Unit::Percent ? c.value / 100.0 : c.value, 0.0, 1.0);
    }

    // 0..1 from a percentage or a number read as one.
    std::optional<double> percent(Component c) {
      if (c.unit != Unit::None && c.unit != Unit::Percent) {
        return std::nullopt;
      }
      return std::clamp(c.value, 0.0, 100.0) / 100.0;
    }

    // Degrees in 0..360.
    std::optional<double> hue(Component c) {
      double degrees = c.value;
      switch (c.unit) {
      case Unit::None:
      case Unit::Deg:
        break;
      case Unit::Rad:
        degrees = c.value * 180.0 / std::numbers::pi;
        break;
      case Unit::Grad:
        degrees = c.value * 0.9;
        break;
      case Unit::Turn:
        degrees = c.value * 360.0;
        break;
      case Unit::Percent:
        return std::nullopt;
      }
      degrees = std::fmod(degrees, 360.0);
      if (!std::isfinite(degrees)) {
        return std::nullopt;
      }
      return degrees < 0.0 ? degrees + 360.0 : degrees;
    }

    std::uint8_t toByte(double unit) {
      return static_cast<std::uint8_t>(std::floor(std::clamp(unit, 0.0, 1.0) * 255.0 + 0.5));
    }

    std::array<double, 3> hslToRgb(double degrees, double saturation, double lightness) {
      const auto f = [&](double n) {
        const double k = std::fmod(n + degrees / 30.0, 12.0);
        const double a = saturation * std::min(lightness, 1.0 - lightness);
        return lightness - a * std::max(-1.0, std::min({k - 3.0, 9.0 - k, 1.0}));
      };
      return {f(0.0), f(8.0), f(4.0)};
    }

    std::optional<std::array<double, 3>> functionRgb(std::string_view name, const Arguments& a, bool commas) {
      if (name.starts_with("rgb")) {
        const auto r = channel(a.parts[0]);
        const auto g = channel(a.parts[1]);
        const auto b = channel(a.parts[2]);
        if (!r || !g || !b) {
          return std::nullopt;
        }
        return std::array{*r / 255.0, *g / 255.0, *b / 255.0};
      }
      const auto h = hue(a.parts[0]);
      const auto first = percent(a.parts[1]);
      const auto second = percent(a.parts[2]);
      if (!h || !first || !second) {
        return std::nullopt;
      }
      if (name.starts_with("hsl")) {
        return hslToRgb(*h, *first, *second);
      }
      if (commas) {
        return std::nullopt;
      }
      const double white = *first;
      const double black = *second;
      if (white + black >= 1.0) {
        const double gray = white / (white + black);
        return std::array{gray, gray, gray};
      }
      auto rgb = hslToRgb(*h, 1.0, 0.5);
      for (auto& part : rgb) {
        part = part * (1.0 - white - black) + white;
      }
      return rgb;
    }
  } // namespace

  std::optional<ColorNotation> cssColorFunction(std::string_view text) {
    std::array<char, 5> lowered{};
    std::size_t nameEnd = 0;
    while (nameEnd < text.size() && nameEnd < lowered.size() && isAsciiAlpha(text[nameEnd])) {
      lowered[nameEnd] = toAsciiLower(text[nameEnd]);
      ++nameEnd;
    }
    const std::string_view name{lowered.data(), nameEnd};
    constexpr std::array<std::string_view, 5> kFunctions{"rgb", "rgba", "hsl", "hsla", "hwb"};
    if (!std::ranges::contains(kFunctions, name) || nameEnd >= text.size() || text[nameEnd] != '(') {
      return std::nullopt;
    }
    const auto close = text.substr(0, kMaxColorFunctionBytes).find(')', nameEnd + 1);
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const auto args = readArguments(text.substr(nameEnd + 1, close - nameEnd - 1));
    if (!args || args->count < 3) {
      return std::nullopt;
    }
    const auto& seps = args->separators;
    const bool commas = seps[0] == ',' && seps[1] == ',' && (args->count == 3 || seps[2] == ',');
    const bool spaces = seps[0] == ' ' && seps[1] == ' ' && (args->count == 3 || seps[2] == '/');
    const auto alpha = args->count == 4 ? fraction(args->parts[3]) : std::optional<double>{1.0};
    const auto rgb = commas || spaces ? functionRgb(name, *args, commas) : std::nullopt;
    if (!rgb || !alpha) {
      return std::nullopt;
    }
    const Rgba color{.r = toByte((*rgb)[0]), .g = toByte((*rgb)[1]), .b = toByte((*rgb)[2]), .a = toByte(*alpha)};
    return ColorNotation{.color = color, .length = close + 1};
  }

  std::optional<Rgba> cssColorName(std::string_view name) {
    if (name.size() > kMaxNameBytes) {
      return std::nullopt;
    }
    std::array<char, kMaxNameBytes> lowered{};
    std::ranges::transform(name, lowered.begin(), toAsciiLower);
    const std::string_view word{lowered.data(), name.size()};
    const auto* const found = std::ranges::lower_bound(kNamedColors, word, {}, &NamedColor::name);
    if (found == kNamedColors.end() || found->name != word) {
      return std::nullopt;
    }
    return found->color;
  }
} // namespace luciole::formats
