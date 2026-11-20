#pragma once

#include "fs/gio_ptr.h"
#include "test_check.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <fstream>
#include <functional>
#include <gio/gio.h>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::test::fonts {
  namespace fs = std::filesystem;
  using Bytes = std::vector<std::uint8_t>;

  inline Bytes readBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  inline void writeBytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  inline std::uint32_t be(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
      value = (value << 8) | bytes[offset + i];
    }
    return value;
  }
  inline void putBe(Bytes& bytes, std::uint32_t value, std::size_t width) {
    for (std::size_t i = width; i > 0; --i) {
      bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
  }
  // Runs a program to completion and returns its standard output.
  inline std::string run(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    const GioPtr<GSubprocess> process(g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDOUT_PIPE, nullptr));
    TEST_CHECK(process != nullptr);
    char* out = nullptr;
    TEST_CHECK(g_subprocess_communicate_utf8(process.get(), nullptr, nullptr, &out, nullptr, nullptr));
    const GString owned(out);
    TEST_CHECK(g_subprocess_get_successful(process.get()));
    return out ? std::string(out) : std::string();
  }
  inline std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    for (auto part : std::views::split(text, separator)) {
      parts.emplace_back(part.begin(), part.end());
    }
    return parts;
  }
  inline bool hasLang(FcPattern* font, const char* lang) {
    FcLangSet* langs = nullptr;
    return FcPatternGetLangSet(font, FC_LANG, 0, &langs) == FcResultMatch
        && FcLangSetHasLang(langs, reinterpret_cast<const FcChar8*>(lang)) == FcLangEqual;
  }
  inline bool flag(FcPattern* font, const char* object) {
    FcBool value = FcFalse;
    return FcPatternGetBool(font, object, 0, &value) == FcResultMatch && value;
  }
  inline bool noLanguages(FcPattern* font) {
    FcLangSet* langs = nullptr;
    if (FcPatternGetLangSet(font, FC_LANG, 0, &langs) != FcResultMatch) {
      return false;
    }
    const std::unique_ptr<FcStrSet, decltype(&FcStrSetDestroy)> names(FcLangSetGetLangs(langs), &FcStrSetDestroy);
    const std::unique_ptr<FcStrList, decltype(&FcStrListDone)> items(FcStrListCreate(names.get()), &FcStrListDone);
    return FcStrListNext(items.get()) == nullptr;
  }

  // Public fonts listed by the default fontconfig configuration: the smallest file of each kind under
  // /usr/share/fonts. A kind the machine lacks is reported as skipped.
  inline std::map<std::string, fs::path> findFonts(std::string_view suite) {
    const std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> any(FcPatternCreate(), &FcPatternDestroy);
    const std::unique_ptr<FcObjectSet, decltype(&FcObjectSetDestroy)> objects(
        FcObjectSetBuild(FC_FILE, FC_INDEX, FC_VARIABLE, FC_COLOR, FC_PIXEL_SIZE, FC_LANG, nullptr), &FcObjectSetDestroy
    );
    const std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> list(
        FcFontList(nullptr, any.get(), objects.get()), &FcFontSetDestroy
    );
    TEST_CHECK(list != nullptr);
    using Accepts = std::function<bool(const fs::path&, FcPattern*)>;
    const auto latin = [](FcPattern* font) { return hasLang(font, "en") && !flag(font, FC_COLOR); };
    const auto extension = [](const fs::path& path, std::initializer_list<std::string_view> names) {
      return std::ranges::contains(names, path.extension().native());
    };
    const std::vector<std::pair<std::string, Accepts>> kinds{
        {"TrueType",
         [&](const auto& path, auto* font) {
           return extension(path, {".ttf"}) && latin(font) && !flag(font, FC_VARIABLE);
         }},
        {"OpenType", [&](const auto& path, auto* font) { return extension(path, {".otf"}) && latin(font); }},
        {"variable", [&](const auto&, auto* font) { return latin(font) && flag(font, FC_VARIABLE); }},
        {"collection", [&](const auto& path, auto* font) { return extension(path, {".ttc", ".otc"}) && latin(font); }},
        {"color", [](const auto&, auto* font) { return flag(font, FC_COLOR); }},
        {"color strikes",
         [](const auto&, auto* font) {
           double size = 0;
           return flag(font, FC_COLOR) && FcPatternGetDouble(font, FC_PIXEL_SIZE, 0, &size) == FcResultMatch;
         }},
        {"non-Latin",
         [](const auto&, auto* font) {
           return (hasLang(font, "ar") || hasLang(font, "he")) && !hasLang(font, "en") && !flag(font, FC_COLOR);
         }},
        {"symbol", [](const auto&, auto* font) { return noLanguages(font) && !flag(font, FC_COLOR); }},
    };
    std::map<std::string, std::pair<std::uintmax_t, fs::path>> best;
    for (int i = 0; i < list->nfont; ++i) {
      FcChar8* file = nullptr;
      int index = 0;
      if (FcPatternGetString(list->fonts[i], FC_FILE, 0, &file) != FcResultMatch
          || FcPatternGetInteger(list->fonts[i], FC_INDEX, 0, &index) != FcResultMatch
          || index != 0) {
        continue;
      }
      const fs::path path(reinterpret_cast<const char*>(file));
      std::error_code error;
      const auto size = fs::file_size(path, error);
      if (!path.native().starts_with("/usr/share/fonts/") || error) {
        continue;
      }
      for (const auto& [kind, accepts] : kinds) {
        auto& slot = best[kind];
        if (accepts(path, list->fonts[i]) && (slot.second.empty() || std::pair(size, path) < slot)) {
          slot = {size, path};
        }
      }
    }
    std::map<std::string, fs::path> found;
    for (const auto& [kind, slot] : best) {
      if (slot.second.empty()) {
        std::println("{}: skipped {}: fontconfig lists no such font under /usr/share/fonts", suite, kind);
      } else {
        std::println("{}: {} font {}", suite, kind, slot.second.native());
        found[kind] = slot.second;
      }
    }
    return found;
  }

  // numGlyphs from the maxp table of an sfnt or of a collection's first face.
  inline std::uint32_t maxpGlyphs(const Bytes& font) {
    const std::size_t start = be(font, 0, 4) == 0x74746366 ? be(font, 12, 4) : 0;
    for (std::uint32_t i = 0; i < be(font, start + 4, 2); ++i) {
      const auto record = start + 12 + (16 * static_cast<std::size_t>(i));
      if (be(font, record, 4) == 0x6D617870) {
        return be(font, be(font, record + 8, 4) + 4, 2);
      }
    }
    return 0;
  }
  // WOFF 1.0 with every table stored uncompressed.
  inline Bytes woffFromSfnt(const Bytes& sfnt) {
    const auto count = be(sfnt, 4, 2);
    std::uint32_t total = 12 + (16 * count);
    const std::uint32_t offset = 44 + (20 * count);
    Bytes directory, data;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto record = 12 + (16 * static_cast<std::size_t>(i));
      const auto start = be(sfnt, record + 8, 4), length = be(sfnt, record + 12, 4);
      for (const auto value :
           {be(sfnt, record, 4), offset + static_cast<std::uint32_t>(data.size()), length, length,
            be(sfnt, record + 4, 4)}) {
        putBe(directory, value, 4);
      }
      data.insert(data.end(), sfnt.begin() + start, sfnt.begin() + start + length);
      data.resize((data.size() + 3) / 4 * 4);
      total += (length + 3) / 4 * 4;
    }
    Bytes woff{'w', 'O', 'F', 'F'};
    putBe(woff, be(sfnt, 0, 4), 4);
    putBe(woff, offset + static_cast<std::uint32_t>(data.size()), 4);
    putBe(woff, count << 16, 4);
    putBe(woff, total, 4);
    putBe(woff, 0x00010000, 4);
    woff.resize(44, 0);
    woff.insert(woff.end(), directory.begin(), directory.end());
    woff.insert(woff.end(), data.begin(), data.end());
    return woff;
  }
  // Web font copies of a TrueType file inside `root`: WOFF always, WOFF2 when woff2_compress is installed.
  inline std::pair<fs::path, std::optional<fs::path>>
  webFonts(std::string_view suite, const fs::path& root, const fs::path& truetype) {
    const auto copy = root / "Web font.ttf";
    fs::copy_file(truetype, copy, fs::copy_options::overwrite_existing);
    writeBytes(root / "Web font.woff", woffFromSfnt(readBytes(copy)));
    const GString compressor(g_find_program_in_path("woff2_compress"));
    if (!compressor) {
      std::println("{}: skipped WOFF2: woff2_compress is not installed", suite);
      return {root / "Web font.woff", std::nullopt};
    }
    (void)run({compressor.get(), copy.native()});
    return {root / "Web font.woff", root / "Web font.woff2"};
  }
} // namespace luciole::test::fonts
