#include "font_fixture.h"
#include "temp_folder.h"
#include "test_check.h"
#include "thumbnails/font_facts.h"
#include "thumbnails/font_sample.h"

#include <algorithm>
#include <random>
#include <set>

namespace {
  using namespace luciole::test::fonts;
  constexpr std::string_view kSuite = "font-facts";

  void containerChecks() {
    using luciole::FontContainer;
    const auto head = [](std::initializer_list<std::uint8_t> bytes) {
      Bytes result(bytes);
      result.resize(64, 0);
      return result;
    };
    // One table record at offset 28, eight bytes long, inside the 64 bytes.
    auto sfnt = head({0, 1, 0, 0, 0, 1});
    sfnt[12 + 11] = 28;
    sfnt[12 + 15] = 8;
    TEST_CHECK(luciole::checkFontFile(sfnt) == FontContainer::Sfnt);
    auto cff = sfnt;
    std::ranges::copy(std::string_view("OTTO"), cff.begin());
    TEST_CHECK(luciole::checkFontFile(cff) == FontContainer::Sfnt);
    auto lying = sfnt;
    lying[12 + 8] = 0x7F;
    TEST_CHECK(!luciole::checkFontFile(lying));
    TEST_CHECK(!luciole::checkFontFile(head({0, 1, 0, 0})));
    TEST_CHECK(!luciole::checkFontFile(head({0, 1, 0, 0, 0xFF, 0xFF})));
    TEST_CHECK(!luciole::checkFontFile(head({'P', 'K', 3, 4})));
    TEST_CHECK(!luciole::checkFontFile({}));
    TEST_CHECK(!luciole::checkFontFile(head({'t', 't', 'c', 'f', 0, 1, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF})));
    // WOFF2 headers must state the true file length and at least one table.
    TEST_CHECK(!luciole::checkFontFile(head({'w', 'O', 'F', '2', 0, 1, 0, 0, 0, 0, 1, 0, 0, 1})));
    TEST_CHECK(!luciole::checkFontFile(head({'w', 'O', 'F', '2', 0, 1, 0, 0, 0, 0, 0, 64})));
    TEST_CHECK(
        luciole::checkFontFile(head({'w', 'O', 'F', '2', 0, 1, 0, 0, 0, 0, 0, 64, 0, 1})) == FontContainer::Woff2
    );
    TEST_CHECK(luciole::isFontContentType("font/ttf") && luciole::isFontContentType("font/woff2"));
    TEST_CHECK(luciole::isFontContentType("application/vnd.oasis.opendocument.formula-template"));
    TEST_CHECK(!luciole::isFontContentType("image/png") && !luciole::isFontContentType(""));
  }

  using CharSet = std::unique_ptr<FcCharSet, decltype(&FcCharSetDestroy)>;
  CharSet charset(std::initializer_list<std::pair<char32_t, char32_t>> ranges) {
    CharSet set(FcCharSetCreate(), &FcCharSetDestroy);
    for (const auto& [first, last] : ranges) {
      for (auto c = first; c <= last; ++c) {
        TEST_CHECK(FcCharSetAddChar(set.get(), c));
      }
    }
    return set;
  }
  bool covered(const FcCharSet* set, std::u32string_view text) {
    return !text.empty() && std::ranges::all_of(text, [&](char32_t c) { return FcCharSetHasChar(set, c); });
  }
  void sampleChecks() {
    const auto ascii = charset({{0x20, 0x7E}});
    TEST_CHECK((luciole::fontScripts(ascii.get()) == std::vector<std::string_view>{"Latin"}));
    const auto latin = luciole::fontSample(ascii.get(), false);
    TEST_CHECK(!latin.grid && latin.text == U"The wizard quickly jinxed the gnomes before they vaporized.");
    TEST_CHECK(luciole::fontSample(ascii.get(), true).grid);
    const auto greek = charset({{0x20, 0x20}, {0x391, 0x3A1}, {0x3A3, 0x3A9}, {0x3B1, 0x3C9}});
    TEST_CHECK((luciole::fontScripts(greek.get()) == std::vector<std::string_view>{"Greek"}));
    const auto greekSample = luciole::fontSample(greek.get(), false);
    TEST_CHECK(!greekSample.grid && covered(greek.get(), greekSample.text));
    TEST_CHECK(std::ranges::none_of(greekSample.text, [](char32_t c) { return c < 0x80 && c != ' '; }));
    const auto icons = charset({{0xF000, 0xF0FF}});
    const auto grid = luciole::fontSample(icons.get(), false);
    TEST_CHECK(luciole::fontScripts(icons.get()).empty() && grid.grid && covered(icons.get(), grid.text));
    TEST_CHECK(grid.text.front() == 0xF000 && grid.text.size() <= 64);
    TEST_CHECK(luciole::fontSample(charset({{1, 0x20}}).get(), false).text.empty());
  }

  // The facts of the first face against fc-scan's report on the same file, and the glyph count against maxp.
  luciole::FontFacts factsMatchScan(const std::string& kind, const fs::path& path, std::uint32_t glyphs) {
    const auto facts = luciole::readFontFacts(readBytes(path));
    TEST_CHECK(facts.has_value());
    const auto scan = run(
        {"fc-scan", "--format",
         "%{index}\t%{family[0]}\t%{style[0]}\t%{fullname[0]}\t%{fontformat}\t%{fontversion}\t%{spacing}\t%{color}\t%{"
         "variable}\t%{lang}\n",
         path.native()}
    );
    std::set<std::uint32_t> faces;
    bool variable = false, first = true;
    for (const auto& line : split(scan, '\n')) {
      const auto field = split(line, '\t');
      if (field.size() != 10) {
        continue;
      }
      const auto index = static_cast<std::uint32_t>(std::stoul(field[0]));
      faces.insert(index & 0xFFFFU);
      variable = variable || field[8] == "True";
      if (!first || index != 0 || field[8] == "True") {
        continue;
      }
      first = false;
      std::println(
          "{}: {} facts: {} / {} / {} / {} scripts", kSuite, kind, facts->family, facts->style, facts->outlines,
          facts->scripts.size()
      );
      TEST_CHECK(facts->family == field[1] && facts->style == field[2] && facts->fullName == field[3]);
      TEST_CHECK(facts->outlines == field[4] && std::to_string(facts->version) == field[5]);
      TEST_CHECK(facts->monospace == (field[6] == "100") && facts->color == (field[7] == "True"));
      const auto langs = split(field[9], '|');
      for (const auto& [lang, name] :
           {std::pair{"en", "Latin"}, {"ru", "Cyrillic"}, {"el", "Greek"}, {"ar", "Arabic"}, {"he", "Hebrew"}}) {
        TEST_CHECK(!std::ranges::contains(langs, lang) || std::ranges::contains(facts->scripts, name));
      }
    }
    TEST_CHECK(!first && facts->faces == static_cast<std::int64_t>(faces.size()));
    TEST_CHECK(facts->axes.empty() != variable && facts->glyphs == glyphs);
    const auto rows = luciole::fontFactFields(*facts);
    const auto row = [&](std::string_view label) {
      const auto found = std::ranges::find(rows, label, &luciole::MetadataField::label);
      return found == rows.end() ? std::string() : found->value;
    };
    TEST_CHECK(row("Family") == facts->family && row("Glyphs") == std::to_string(glyphs));
    TEST_CHECK(row("Faces") == std::to_string(faces.size()) && row("Axes").empty() == !variable);
    TEST_CHECK(row("Monospaced") == (facts->monospace ? "Yes" : "No") && !row("Format").empty());
    return *facts;
  }

  void hostileChecks(const Bytes& good) {
    std::mt19937 random(7);
    Bytes noise(64U * 1024U);
    std::ranges::generate(noise, [&] { return static_cast<std::uint8_t>(random()); });
    TEST_CHECK(
        !luciole::readFontFacts(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2)))
    );
    TEST_CHECK(!luciole::readFontFacts({}));
    TEST_CHECK(!luciole::readFontFacts(noise));
  }
} // namespace

int main() {
  containerChecks();
  sampleChecks();
  const auto fonts = findFonts(kSuite);
  for (const auto& [kind, path] : fonts) {
    const auto facts = factsMatchScan(kind, path, maxpGlyphs(readBytes(path)));
    const bool colored = kind.starts_with("color");
    TEST_CHECK(colored || kind == "non-Latin" || kind == "symbol" || std::ranges::contains(facts.scripts, "Latin"));
    TEST_CHECK(kind != "non-Latin" || (!facts.scripts.empty() && !std::ranges::contains(facts.scripts, "Latin")));
    TEST_CHECK(kind != "symbol" || facts.scripts.empty());
    TEST_CHECK(!colored || facts.color);
  }
  const auto truetype = fonts.find("TrueType");
  if (truetype == fonts.end()) {
    std::println("{}: skipped WOFF, WOFF2 and damaged-file checks: no TrueType font", kSuite);
    return 0;
  }
  const luciole::test::TempFolder temporary("luciole-font-facts");
  const fs::path& root = temporary.path();
  const auto glyphs = maxpGlyphs(readBytes(truetype->second));
  const auto [woff, woff2] = webFonts(kSuite, root, truetype->second);
  TEST_CHECK(factsMatchScan("WOFF", woff, glyphs).container == luciole::FontContainer::Woff);
  if (woff2) {
    TEST_CHECK(factsMatchScan("WOFF2", *woff2, glyphs).container == luciole::FontContainer::Woff2);
  }
  hostileChecks(readBytes(truetype->second));
  std::println("font facts checks passed");
}
