#include "thumbnails/font_sample.h"

#include <algorithm>
#include <array>
#include <glib.h>
#include <pango/pango.h>

namespace {
  struct Range {
    char32_t first = 0;
    char32_t last = 0;
  };
  // Letters that identify a script, and the language whose Pango sample sentence shows it.
  struct Script {
    std::string_view name;
    std::string_view language;
    std::array<Range, 2> letters;
  };
  constexpr std::array<Script, 15> kScripts{{
      {.name = "Latin", .language = "en", .letters = {{{0x41, 0x5A}, {0x61, 0x7A}}}},
      {.name = "Greek", .language = "el", .letters = {{{0x391, 0x3A9}, {0x3B1, 0x3C9}}}},
      {.name = "Cyrillic", .language = "ru", .letters = {{{0x410, 0x44F}, {}}}},
      {.name = "Armenian", .language = "hy", .letters = {{{0x531, 0x556}, {0x561, 0x586}}}},
      {.name = "Hebrew", .language = "he", .letters = {{{0x5D0, 0x5EA}, {}}}},
      {.name = "Arabic", .language = "ar", .letters = {{{0x621, 0x64A}, {}}}},
      {.name = "Devanagari", .language = "hi", .letters = {{{0x905, 0x939}, {}}}},
      {.name = "Bengali", .language = "bn", .letters = {{{0x985, 0x9B9}, {}}}},
      {.name = "Tamil", .language = "ta", .letters = {{{0xB85, 0xBB9}, {}}}},
      {.name = "Thai", .language = "th", .letters = {{{0xE01, 0xE2E}, {}}}},
      {.name = "Georgian", .language = "ka", .letters = {{{0x10D0, 0x10FA}, {}}}},
      {.name = "Ethiopic", .language = "am", .letters = {{{0x1200, 0x135A}, {}}}},
      {.name = "Kana", .language = "ja", .letters = {{{0x3041, 0x3096}, {0x30A1, 0x30FA}}}},
      {.name = "Hangul", .language = "ko", .letters = {{{0xAC00, 0xD7A3}, {}}}},
      {.name = "Han", .language = "zh-cn", .letters = {{{0x4E00, 0x9FFF}, {}}}},
  }};
  constexpr std::size_t kMaxSampleCharacters = 48;
  constexpr std::size_t kMaxGridCharacters = 48;
  // 0x110000 code points in pages of 256.
  constexpr std::size_t kMaxCharSetPages = 0x1100;

  bool covers(const FcCharSet* coverage, const Script& script) {
    std::size_t total = 0, covered = 0;
    for (const auto& range : script.letters) {
      for (auto c = range.first; range.first != 0 && c <= range.last; ++c) {
        ++total;
        covered += FcCharSetHasChar(coverage, c) ? 1U : 0U;
      }
    }
    return total != 0 && covered * 2 >= total;
  }
  bool drawable(char32_t c) {
    switch (g_unichar_type(c)) {
    case G_UNICODE_CONTROL:
    case G_UNICODE_FORMAT:
    case G_UNICODE_UNASSIGNED:
    case G_UNICODE_SURROGATE:
    case G_UNICODE_SPACE_SEPARATOR:
    case G_UNICODE_LINE_SEPARATOR:
    case G_UNICODE_PARAGRAPH_SEPARATOR:
      return false;
    default:
      return true;
    }
  }
  // Pango's sentence for the script when every character of it is covered, else its first covered letters.
  std::u32string scriptText(const FcCharSet* coverage, const Script& script) {
    std::u32string text;
    const char* sentence = pango_language_get_sample_string(pango_language_from_string(script.language.data()));
    for (const char* p = sentence; p && *p && text.size() <= kMaxSampleCharacters * 2; p = g_utf8_next_char(p)) {
      text.push_back(g_utf8_get_char(p));
    }
    const bool whole = !text.empty()
        && text.size() <= kMaxSampleCharacters * 2
        && std::ranges::all_of(text, [&](char32_t c) { return FcCharSetHasChar(coverage, c); });
    if (whole) {
      return text;
    }
    text.clear();
    for (const auto& range : script.letters) {
      for (auto c = range.first; range.first != 0 && c <= range.last && text.size() < kMaxSampleCharacters; ++c) {
        if (FcCharSetHasChar(coverage, c)) {
          text.push_back(c);
        }
      }
    }
    return text;
  }
  std::u32string gridText(const FcCharSet* coverage) {
    std::u32string text;
    std::array<FcChar32, FC_CHARSET_MAP_SIZE> map{};
    FcChar32 next = 0;
    auto base = FcCharSetFirstPage(coverage, map.data(), &next);
    for (std::size_t page = 0; page < kMaxCharSetPages && base != FC_CHARSET_DONE; ++page) {
      for (std::size_t word = 0; word < map.size(); ++word) {
        for (std::size_t bit = 0; bit < 32 && text.size() < kMaxGridCharacters; ++bit) {
          const auto c = static_cast<char32_t>(base + (word * 32) + bit);
          if ((map[word] >> bit) & 1U && drawable(c)) {
            text.push_back(c);
          }
        }
      }
      if (text.size() >= kMaxGridCharacters) {
        break;
      }
      base = FcCharSetNextPage(coverage, map.data(), &next);
    }
    return text;
  }
} // namespace

std::vector<std::string_view> luciole::fontScripts(const FcCharSet* coverage) {
  std::vector<std::string_view> names;
  for (const auto& script : kScripts) {
    if (coverage && covers(coverage, script)) {
      names.push_back(script.name);
    }
  }
  return names;
}

luciole::FontSample luciole::fontSample(const FcCharSet* coverage, bool symbol) {
  if (!coverage) {
    return {.text = {}, .grid = true};
  }
  if (!symbol) {
    for (const auto& script : kScripts) {
      if (covers(coverage, script)) {
        return {.text = scriptText(coverage, script), .grid = false};
      }
    }
  }
  return {.text = gridText(coverage), .grid = true};
}
