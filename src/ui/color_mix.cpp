#include "ui/color_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace {
  struct Lab {
    float l;
    float a;
    float b;
  };

  using Rgb = std::array<float, 3>;

  float decode(float channel) {
    return channel <= 0.04045F ? channel / 12.92F : std::pow((channel + 0.055F) / 1.055F, 2.4F);
  }

  float encode(float linear) {
    const float clamped = std::clamp(linear, 0.0F, 1.0F);
    return clamped <= 0.0031308F ? clamped * 12.92F : (1.055F * std::pow(clamped, 1.0F / 2.4F)) - 0.055F;
  }

  Lab toLab(const Color& color) {
    const float r = decode(color.r);
    const float g = decode(color.g);
    const float b = decode(color.b);
    const float l = std::cbrt((0.4122214708F * r) + (0.5363325363F * g) + (0.0514459929F * b));
    const float m = std::cbrt((0.2119034982F * r) + (0.6806995451F * g) + (0.1073969566F * b));
    const float s = std::cbrt((0.0883024619F * r) + (0.2817188376F * g) + (0.6299787005F * b));
    return {
        .l = (0.2104542553F * l) + (0.7936177850F * m) - (0.0040720468F * s),
        .a = (1.9779984951F * l) - (2.4285922050F * m) + (0.4505937099F * s),
        .b = (0.0259040371F * l) + (0.7827717662F * m) - (0.8086757660F * s),
    };
  }

  // Linear sRGB of a Lab color; channels may fall outside [0,1].
  Rgb toLinear(const Lab& lab) {
    const float l = std::pow(lab.l + (0.3963377774F * lab.a) + (0.2158037573F * lab.b), 3.0F);
    const float m = std::pow(lab.l - (0.1055613458F * lab.a) - (0.0638541728F * lab.b), 3.0F);
    const float s = std::pow(lab.l - (0.0894841775F * lab.a) - (1.2914855480F * lab.b), 3.0F);
    return {
        (4.0767416621F * l) - (3.3077115913F * m) + (0.2309699292F * s),
        (-1.2684380046F * l) + (2.6097574011F * m) - (0.3413193965F * s),
        (-0.0041960863F * l) - (0.7034186147F * m) + (1.7076147010F * s),
    };
  }

  Color fromLab(const Lab& lab) {
    const Rgb rgb = toLinear(lab);
    return rgba(encode(rgb[0]), encode(rgb[1]), encode(rgb[2]));
  }

  bool inGamut(const Lab& lab) {
    constexpr float kSlack = 0.0005F;
    return std::ranges::all_of(toLinear(lab), [](float v) { return v >= -kSlack && v <= 1.0F + kSlack; });
  }
} // namespace

Color luciole::mixOklab(const Color& a, const Color& b, float t) {
  const Lab from = toLab(a);
  const Lab to = toLab(b);
  return fromLab({
      .l = from.l + ((to.l - from.l) * t),
      .a = from.a + ((to.a - from.a) * t),
      .b = from.b + ((to.b - from.b) * t),
  });
}

Color luciole::rotateHue(const Color& base, float turns, float chromaScale) {
  const Lab lab = toLab(base);
  const float chroma = std::hypot(lab.a, lab.b) * chromaScale;
  const float hue = std::atan2(lab.b, lab.a) + (turns * 2.0F * std::numbers::pi_v<float>);
  const auto at = [&](float scale) {
    return Lab{.l = lab.l, .a = std::cos(hue) * chroma * scale, .b = std::sin(hue) * chroma * scale};
  };
  if (inGamut(at(1.0F))) {
    return fromLab(at(1.0F));
  }
  // The most chroma that still fits sRGB at this lightness and hue: a fixed number of halvings.
  float low = 0.0F;
  float high = 1.0F;
  for (int step = 0; step < 16; ++step) {
    const float mid = (low + high) / 2.0F;
    (inGamut(at(mid)) ? low : high) = mid;
  }
  return fromLab(at(low));
}

float luciole::contrastRatio(const Color& a, const Color& b) {
  const float la = relativeLuminance(a);
  const float lb = relativeLuminance(b);
  return (std::max(la, lb) + 0.05F) / (std::min(la, lb) + 0.05F);
}

float luciole::oklabDistance(const Color& a, const Color& b) {
  const Lab x = toLab(a);
  const Lab y = toLab(b);
  return std::sqrt(((x.l - y.l) * (x.l - y.l)) + ((x.a - y.a) * (x.a - y.a)) + ((x.b - y.b) * (x.b - y.b)));
}
