#pragma once

#include <fontconfig/fontconfig.h>
#include <memory>

namespace luciole::test {
  // True when an installed font has a glyph for `character`. A test of a fallback face skips on a machine whose
  // fonts have none, such as a container with only the Latin Noto fonts.
  inline bool installedFontCovers(FcChar32 character) {
    const std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)> set(FcCharSetCreate(), &FcCharSetDestroy);
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> wanted(FcPatternCreate(), &FcPatternDestroy);
    const std::unique_ptr<FcObjectSet, decltype(&FcObjectSetDestroy)> objects(
        FcObjectSetBuild(FC_FILE, nullptr), &FcObjectSetDestroy
    );
    if (!FcCharSetAddChar(set.get(), character) || !FcPatternAddCharSet(wanted.get(), FC_CHARSET, set.get())) {
      return false;
    }
    const std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> fonts(
        FcFontList(nullptr, wanted.get(), objects.get()), &FcFontSetDestroy
    );
    return fonts && fonts->nfont > 0;
  }
} // namespace luciole::test
