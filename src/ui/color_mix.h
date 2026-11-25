#pragma once

#include "render/core/color.h"

namespace luciole {
  // Mixes in Oklab, a perceptually even space, so equal steps look equal in every hue and brightness. t in [0,1]
  // runs from a to b; the result is opaque.
  Color mixOklab(const Color& a, const Color& b, float t);
  // Moves the hue of base by turns (1.0 is a full circle) and scales its chroma, in Oklch at the same lightness;
  // chroma is reduced where needed to stay inside sRGB. The result is opaque.
  Color rotateHue(const Color& base, float turns, float chromaScale);
  // WCAG contrast ratio of two colors, from 1 to 21; alpha is ignored.
  float contrastRatio(const Color& a, const Color& b);
  // Distance of two colors in Oklab.
  float oklabDistance(const Color& a, const Color& b);
} // namespace luciole
