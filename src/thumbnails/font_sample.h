#pragma once

#include <fontconfig/fontconfig.h>
#include <string>
#include <string_view>
#include <vector>

namespace luciole {
  // Names of the scripts whose letters the coverage holds at least half of, in a fixed order starting with Latin.
  std::vector<std::string_view> fontScripts(const FcCharSet* coverage);

  struct FontSample {
    // Characters the coverage holds; empty when the font has nothing drawable.
    std::u32string text;
    // Each character is drawn in its own cell instead of as running text.
    bool grid = false;
  };

  // Text in the first covered script, or a grid of the first drawable characters for symbol fonts and fonts that
  // cover no script.
  FontSample fontSample(const FcCharSet* coverage, bool symbol);
} // namespace luciole
