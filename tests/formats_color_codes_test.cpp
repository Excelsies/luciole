#include "formats/color_codes.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  constexpr Rgba rgba(int r, int g, int b, int a = 255) {
    return {
        .r = static_cast<std::uint8_t>(r),
        .g = static_cast<std::uint8_t>(g),
        .b = static_cast<std::uint8_t>(b),
        .a = static_cast<std::uint8_t>(a)
    };
  }

  FileKind kindOf(Language language, CommentStyle comment = CommentStyle::None) {
    return {.content = ContentKind::Text, .language = language, .comment = comment};
  }

  std::vector<ColorMatch> scan(std::string_view line, Language language, bool names = false) {
    return findLineColors(line, kindOf(language), names);
  }

  std::vector<ColorMatch> css(std::string_view line, bool names = false) { return scan(line, Language::Css, names); }

  std::vector<std::string> texts(std::string_view line, const std::vector<ColorMatch>& matches) {
    std::vector<std::string> out;
    out.reserve(matches.size());
    for (const auto& match : matches) {
      out.emplace_back(line.substr(match.offset, match.length));
    }
    return out;
  }

  // The line holds exactly one code, which reads as `expected`.
  bool one(std::string_view line, std::string_view code, Rgba expected, Language language = Language::Css) {
    const auto found = scan(line, language);
    return found.size() == 1 && line.substr(found[0].offset, found[0].length) == code && found[0].color == expected;
  }

  bool none(std::string_view line, Language language = Language::Css, bool names = false) {
    return scan(line, language, names).empty();
  }

  void testHexNotations() {
    TEST_CHECK(one("color: #fff;", "#fff", rgba(255, 255, 255)));
    TEST_CHECK(one("color: #f80;", "#f80", rgba(255, 136, 0)));
    TEST_CHECK(one("a: #abcd", "#abcd", rgba(0xAA, 0xBB, 0xCC, 0xDD)));
    TEST_CHECK(one("a: #1a2b3c", "#1a2b3c", rgba(0x1A, 0x2B, 0x3C)));
    TEST_CHECK(one("a: #1a2b3c80", "#1a2b3c80", rgba(0x1A, 0x2B, 0x3C, 0x80)));
    TEST_CHECK(one("a: #ABCDEF", "#ABCDEF", rgba(0xAB, 0xCD, 0xEF)));
    TEST_CHECK(one("a: #00000000", "#00000000", rgba(0, 0, 0, 0)));
    const auto found = css("color: #fff;");
    TEST_CHECK(found.size() == 1 && found[0].offset == 7 && found[0].length == 4);
    TEST_CHECK((texts("a{b:#000;c:#fff}", css("a{b:#000;c:#fff}")) == std::vector<std::string>{"#000", "#fff"}));
  }

  void testRgb() {
    TEST_CHECK(one("c: rgb(255, 0, 0);", "rgb(255, 0, 0)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: rgb(255 0 0);", "rgb(255 0 0)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: rgba(0, 0, 255, 0.5)", "rgba(0, 0, 255, 0.5)", rgba(0, 0, 255, 128)));
    TEST_CHECK(one("c: rgb(0 255 0 / 50%)", "rgb(0 255 0 / 50%)", rgba(0, 255, 0, 128)));
    TEST_CHECK(one("c: rgb(0 255 0/.25)", "rgb(0 255 0/.25)", rgba(0, 255, 0, 64)));
    TEST_CHECK(one("c: rgba(1, 2, 3)", "rgba(1, 2, 3)", rgba(1, 2, 3)));
    TEST_CHECK(one("c: rgb(1, 2, 3, 0)", "rgb(1, 2, 3, 0)", rgba(1, 2, 3, 0)));
    TEST_CHECK(one("c: rgb(100%, 50%, 0%)", "rgb(100%, 50%, 0%)", rgba(255, 128, 0)));
    TEST_CHECK(one("c: rgba(10,20,30,.25)", "rgba(10,20,30,.25)", rgba(10, 20, 30, 64)));
    TEST_CHECK(one("c: RGB(1,2,3)", "RGB(1,2,3)", rgba(1, 2, 3)));
    TEST_CHECK(one("c: rgb( 1 , 2 , 3 )", "rgb( 1 , 2 , 3 )", rgba(1, 2, 3)));
    TEST_CHECK(one("c: rgb(1e2, +5, 12.6)", "rgb(1e2, +5, 12.6)", rgba(100, 5, 13)));
    TEST_CHECK(one("c: rgb(0 0 0 / 1E-1)", "rgb(0 0 0 / 1E-1)", rgba(0, 0, 0, 26)));
  }

  void testOutOfRangeClamps() {
    TEST_CHECK(one("c: rgb(300, -20, 128)", "rgb(300, -20, 128)", rgba(255, 0, 128)));
    TEST_CHECK(one("c: rgb(150% 0% -5%)", "rgb(150% 0% -5%)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: rgba(0,0,0,2)", "rgba(0,0,0,2)", rgba(0, 0, 0, 255)));
    TEST_CHECK(one("c: rgba(0,0,0,-1)", "rgba(0,0,0,-1)", rgba(0, 0, 0, 0)));
    TEST_CHECK(one("c: rgb(0 0 0 / 150%)", "rgb(0 0 0 / 150%)", rgba(0, 0, 0, 255)));
    TEST_CHECK(one("c: hsl(0, 150%, 120%)", "hsl(0, 150%, 120%)", rgba(255, 255, 255)));
    TEST_CHECK(one("c: hsl(0, -10%, 50%)", "hsl(0, -10%, 50%)", rgba(128, 128, 128)));
    TEST_CHECK(one("c: hsl(-120, 100%, 50%)", "hsl(-120, 100%, 50%)", rgba(0, 0, 255)));
    TEST_CHECK(one("c: hsl(480, 100%, 50%)", "hsl(480, 100%, 50%)", rgba(0, 255, 0)));
    TEST_CHECK(one("c: hwb(0 80% 70%)", "hwb(0 80% 70%)", rgba(136, 136, 136)));
    // Values too large for a double are not colors.
    TEST_CHECK(none("c: rgb(1e999, 0, 0)"));
    TEST_CHECK(none("c: hsl(1e999, 100%, 50%)"));
  }

  void testHsl() {
    TEST_CHECK(one("c: hsl(120, 100%, 50%)", "hsl(120, 100%, 50%)", rgba(0, 255, 0)));
    TEST_CHECK(one("c: hsl(30 100% 50%)", "hsl(30 100% 50%)", rgba(255, 128, 0)));
    TEST_CHECK(one("c: hsl(90deg 100% 50%)", "hsl(90deg 100% 50%)", rgba(128, 255, 0)));
    TEST_CHECK(one("c: hsla(240, 100%, 50%, 0.5)", "hsla(240, 100%, 50%, 0.5)", rgba(0, 0, 255, 128)));
    TEST_CHECK(one("c: hsl(240 100% 50% / 25%)", "hsl(240 100% 50% / 25%)", rgba(0, 0, 255, 64)));
    TEST_CHECK(one("c: hsl(0.5turn 100% 50%)", "hsl(0.5turn 100% 50%)", rgba(0, 255, 255)));
    TEST_CHECK(one("c: hsl(200grad 100% 50%)", "hsl(200grad 100% 50%)", rgba(0, 255, 255)));
    TEST_CHECK(one("c: hsl(3.14159265rad 100% 50%)", "hsl(3.14159265rad 100% 50%)", rgba(0, 255, 255)));
    TEST_CHECK(one("c: HSL(0DEG 100% 50%)", "HSL(0DEG 100% 50%)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: hsl(0 100 50)", "hsl(0 100 50)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: hsl(0, 0%, 20%)", "hsl(0, 0%, 20%)", rgba(51, 51, 51)));
  }

  void testHwb() {
    TEST_CHECK(one("c: hwb(0 0% 0%)", "hwb(0 0% 0%)", rgba(255, 0, 0)));
    TEST_CHECK(one("c: hwb(120 20% 20%)", "hwb(120 20% 20%)", rgba(51, 204, 51)));
    TEST_CHECK(one("c: hwb(0 60% 60%)", "hwb(0 60% 60%)", rgba(128, 128, 128)));
    TEST_CHECK(one("c: hwb(240 0% 0% / 0.25)", "hwb(240 0% 0% / 0.25)", rgba(0, 0, 255, 64)));
    TEST_CHECK(none("c: hwb(0, 0%, 0%)")); // no comma syntax for hwb
  }

  void testMalformedFunctions() {
    for (const auto* line :
         {"c: rgb(1,2)",
          "c: rgb(1,2,3,4,5)",
          "c: rgb(1 2, 3)",
          "c: rgb(1, 2 3)",
          "c: rgb(1,2,3 / 4)",
          "c: rgb(1 2 3 4)",
          "c: rgb(1 2 / 3 / 4)",
          "c: rgb(1 / 2 3)",
          "c: rgb(1deg,2,3)",
          "c: rgb(1,2,3,4deg)",
          "c: hsl(10%, 50%, 50%)",
          "c: hsl(10, 50deg, 50%)",
          "c: rgb(var(--x))",
          "c: rgb (1,2,3)",
          "c: rgb(1,2,3",
          "c: rgb(,1,2,3)",
          "c: rgb(1,2,3,)",
          "c: rgb(1,,2,3)",
          "c: rgb(1px,2,3)",
          "c: rgb(1e,2,3)",
          "c: rgb(.,2,3)",
          "c: rgb(-,2,3)",
          "c: rgb()",
          "c: rgb",
          "c: rgbx(1,2,3)",
          "c: myrgb(1,2,3)",
          "c: x-rgb(1,2,3)",
          "c: hsv(1,2,3)",
          "c: rgb(1 2 3 /)",
          "c: rgb(1+2,3,4)",
          "c: hsl(1e306turn 100% 50%)"}) {
      TEST_CHECK(none(line));
    }
    // A function longer than the limit is not read.
    TEST_CHECK(none("c: rgb(" + std::string(200, ' ') + "1,2,3)"));
    TEST_CHECK(!css("c: rgb(" + std::string(100, ' ') + "1,2,3)").empty());
  }

  void testHexLiteralsInCode() {
    const auto code = [](std::string_view line, std::string_view text, Rgba color) {
      const auto found = findLineColors(line, kindOf(Language::Cpp, CommentStyle::Slash), false);
      return found.size() == 1 && line.substr(found[0].offset, found[0].length) == text && found[0].color == color;
    };
    TEST_CHECK(code("int c = 0xFF0000;", "0xFF0000", rgba(255, 0, 0)));
    TEST_CHECK(code("int c = 0x80FF0000;", "0x80FF0000", rgba(255, 0, 0, 0x80)));
    TEST_CHECK(code("c = 0xff00ffU;", "0xff00ff", rgba(255, 0, 255)));
    TEST_CHECK(code("c = 0xFF00FFULL;", "0xFF00FF", rgba(255, 0, 255)));
    TEST_CHECK(code("c = 0X00ff00;", "0X00ff00", rgba(0, 255, 0)));
    TEST_CHECK(code("s = \"0x0000ff\";", "0x0000ff", rgba(0, 0, 255)));
    for (const auto* line :
         {"c = 0x00FF00FF00;", "c = 0xFFF;", "c = 0xFF00F;", "c = a0xFF0000;", "c = 1.0xFF0000;", "c = 0xFF0000z;",
          "c = 0xFF0000_;", "c = 0xFF0000UUUU;", "c = 0xFF_00_FF;", "c = 0x;"}) {
      TEST_CHECK(findLineColors(line, kindOf(Language::Cpp, CommentStyle::Slash), false).empty());
    }
    // Only code reads 0x literals.
    TEST_CHECK(none("a: 0xFF0000", Language::Css));
    TEST_CHECK(none("a = 0xFF0000", Language::Toml));
  }

  void testCodeStringsOnly() {
    const auto js = kindOf(Language::EcmaScript, CommentStyle::Slash);
    const std::string_view line = R"(const a = "#ff0000", b = #00ff00; // "#0000ff")";
    TEST_CHECK((texts(line, findLineColors(line, js, false)) == std::vector<std::string>{"#ff0000"}));
    const std::string_view block = R"(x = 'rgb(1,2,3)' /* "#fff" */)";
    TEST_CHECK((texts(block, findLineColors(block, js, false)) == std::vector<std::string>{"rgb(1,2,3)"}));
    const std::string_view tick = "t = `#abc` + rgb(1,2,3)";
    TEST_CHECK((texts(tick, findLineColors(tick, js, false)) == std::vector<std::string>{"#abc"}));
    const std::string_view escaped = R"(s = "a\"#fff\" b")";
    TEST_CHECK((texts(escaped, findLineColors(escaped, js, false)) == std::vector<std::string>{"#fff"}));
    const std::string_view unclosed = R"(s = "#fff)";
    TEST_CHECK(findLineColors(unclosed, js, false).size() == 1);

    const auto py = kindOf(Language::Python, CommentStyle::Hash);
    const std::string_view pyLine = "c = '#abc'  # '#def'";
    TEST_CHECK((texts(pyLine, findLineColors(pyLine, py, false)) == std::vector<std::string>{"#abc"}));
    const std::string_view shell = R"(echo "#fff" #comment "#000")";
    TEST_CHECK(findLineColors(shell, kindOf(Language::Shell, CommentStyle::Hash), false).size() == 1);
    const std::string_view sql = "select '#fff' -- '#000'";
    TEST_CHECK(findLineColors(sql, kindOf(Language::Sql, CommentStyle::Dash), false).size() == 1);
    const std::string_view generic = R"(a = "#fff" // "#000")";
    TEST_CHECK(findLineColors(generic, kindOf(Language::Code), false).size() == 1);
    const std::string_view semi = R"(a = "#fff" ; "#000")";
    TEST_CHECK(findLineColors(semi, kindOf(Language::Code, CommentStyle::Semi), false).size() == 1);
    TEST_CHECK(findLineColors(semi, kindOf(Language::Code, CommentStyle::Hash), false).size() == 2);

    // Digits alone in a code string are a color only when they are the whole string.
    const auto whole = [&](std::string_view text) { return findLineColors(text, js, false).size(); };
    TEST_CHECK(whole(R"(c = "#123")") == 1);
    TEST_CHECK(whole(R"(c = '#123456')") == 1);
    TEST_CHECK(whole(R"(m = "Fixes #1234")") == 0);
    TEST_CHECK(whole(R"(m = "#1234 fixed")") == 0);
    TEST_CHECK(whole(R"(m = "#123456 ")") == 0);
    TEST_CHECK(whole(R"(m = "see #12ab")") == 1);
  }

  void testLanguages() {
    for (const auto language :
         {Language::None, Language::Plain, Language::Markdown, Language::Diff, Language::Csv, Language::Tsv,
          static_cast<Language>(200)}) {
      TEST_CHECK(none("a #fff rgb(1,2,3) red 0xFF0000", language, true));
      TEST_CHECK(!colorLanguage(language));
    }
    TEST_CHECK(one(R"({"bg": "#1234"})", "#1234", rgba(0x11, 0x22, 0x33, 0x44), Language::Json));
    TEST_CHECK(one(R"({"bg": "#fff"})", "#fff", rgba(255, 255, 255), Language::JsonLines));
    TEST_CHECK(one("color: \"#fff\"", "#fff", rgba(255, 255, 255), Language::Yaml));
    TEST_CHECK(one("color = \"#fff\"", "#fff", rgba(255, 255, 255), Language::Toml));
    TEST_CHECK(one("color=#fff", "#fff", rgba(255, 255, 255), Language::Ini));
    TEST_CHECK(one("Color=#fff", "#fff", rgba(255, 255, 255), Language::DesktopEntry));
    TEST_CHECK(one(R"(<c v="#fff"/>)", "#fff", rgba(255, 255, 255), Language::Xml));
    TEST_CHECK(one(R"(<p style="color:#fade">)", "#fade", rgba(0xFF, 0xAA, 0xDD, 0xEE), Language::Html));
    TEST_CHECK(one("$c: #fff;", "#fff", rgba(255, 255, 255), Language::Scss));
    TEST_CHECK(one("notes: fixes #1234", "#1234", rgba(0x11, 0x22, 0x33, 0x44), Language::Yaml));
    for (const auto language :
         {Language::Css, Language::Scss, Language::Json, Language::JsonLines, Language::Toml, Language::Yaml,
          Language::Ini, Language::DesktopEntry, Language::Xml, Language::Html}) {
      TEST_CHECK(colorLanguage(language));
    }
    for (const auto language : {Language::Code, Language::Cpp, Language::Python, Language::Shell}) {
      TEST_CHECK(!colorLanguage(language));
    }
  }

  void testHashGuards() {
    // Inside a longer word or hexadecimal run.
    for (const auto* line :
         {"c: a#fff", "c: abc#fff", "c: #fffg", "c: #fff-dark", "c: #fff_x", "c: #ggg", "c: #12345", "c: #1234567",
          "c: #123456789", "c: ##fff", "c: #", "c: #-fff", "c: \\#fff"}) {
      TEST_CHECK(none(line));
    }
    // Entities and paths.
    TEST_CHECK(none("x &#123; y", Language::Html));
    TEST_CHECK(none("p: a/#fff", Language::Yaml));
    // The first thing on a line is a comment, heading or directive, except in CSS where values can wrap.
    TEST_CHECK(none("#add more", Language::Yaml));
    TEST_CHECK(none("  #fff", Language::Yaml));
    TEST_CHECK(none("#fff", Language::Ini));
    TEST_CHECK(none("\t#fff", Language::Toml));
    TEST_CHECK(one("  #fff,", "#fff", rgba(255, 255, 255)));
    // CSS selectors are not colors.
    for (const auto* line :
         {"#fade { color: red }", "#bad:hover {", "#abc.x {", "#fff > a {", "a, #fed {", "#cab[x] {", "#dad + p",
          "#bee ~ p"}) {
      TEST_CHECK(none(line));
    }
    TEST_CHECK(none("#bad {", Language::Html));
    // URL fragments.
    TEST_CHECK(none("u: http://example.com/#fade", Language::Yaml));
    TEST_CHECK(none("u: http://x.com/a?b#fade", Language::Yaml));
    TEST_CHECK(none("u: http://x.com/?#fade", Language::Yaml));
    TEST_CHECK(none("u: \"https://x.com:80/p?q=#fade\"", Language::Json));
    TEST_CHECK(none(R"(<a href="#fade">)", Language::Html));
    TEST_CHECK(none(R"(<a HREF = '#fade'>)", Language::Html));
    TEST_CHECK(none(R"(<img src="#fff">)", Language::Html));
    TEST_CHECK(none(R"(<use xlink:href="#cafe"/>)", Language::Xml));
    TEST_CHECK(none("fill: url(#fade);"));
    TEST_CHECK(none("fill: url(\"#fade\");"));
    TEST_CHECK(none("fill: URL('#fade');"));
    // A URL ends at a space, so a code after it still counts.
    TEST_CHECK(one("u: http://x.com/ #fade", "#fade", rgba(0xFF, 0xAA, 0xDD, 0xEE), Language::Yaml));
    // Issue numbers outside color languages are refused.
    TEST_CHECK(findLineColors(R"(m = "Fixes #1234")", kindOf(Language::Rust, CommentStyle::Slash), false).empty());
  }

  void testNames() {
    TEST_CHECK(none("color: red;"));
    const auto red = css("color: red;", true);
    TEST_CHECK(red.size() == 1 && red[0].offset == 7 && red[0].length == 3 && red[0].color == rgba(255, 0, 0));
    TEST_CHECK(css("color: Red;", true).size() == 1);
    TEST_CHECK(css("color: transparent;", true)[0].color == rgba(0, 0, 0, 0));
    TEST_CHECK(css("color: rebeccapurple;", true)[0].color == rgba(102, 51, 153));
    TEST_CHECK(css("color: lightgoldenrodyellow;", true)[0].color == rgba(250, 250, 210));
    TEST_CHECK(css("color: aliceblue;", true)[0].color == rgba(240, 248, 255));
    TEST_CHECK(css("color: yellowgreen;", true)[0].color == rgba(154, 205, 50));
    TEST_CHECK(css("color: grey;", true)[0].color == rgba(128, 128, 128));
    TEST_CHECK(css("color: darkslategrey;", true)[0].color == rgba(47, 79, 79));
    const std::string purpleName = std::string{"mag"} + "enta";
    TEST_CHECK(css("color: " + purpleName + ";", true).at(0).color == rgba(255, 0, 255));
    TEST_CHECK(css("color: dark" + purpleName + ";", true).at(0).color == rgba(139, 0, 139));
    for (const auto* line :
         {"c: darkred-x", "c: .red", "c: $red", "c: @red", "c: #red", "c: red(", "red: 1", "c: reddish", "c: bored",
          "c: red_x", "c: x-red", "c: redd", "c: currentcolor"}) {
      TEST_CHECK(scan(line, Language::Css, true).empty());
    }
    TEST_CHECK(
        (texts("border: 1px solid red; color: blue", css("border: 1px solid red; color: blue", true))
         == std::vector<std::string>{"red", "blue"})
    );
    // In code, names count only inside strings.
    const auto py = kindOf(Language::Python, CommentStyle::Hash);
    TEST_CHECK(findLineColors(R"(color = "red")", py, true).size() == 1);
    TEST_CHECK(findLineColors(R"(color = "red")", py, false).empty());
    TEST_CHECK(findLineColors("red = 1", py, true).empty());
  }

  void testBounds() {
    std::string line;
    for (int i = 0; i < 100; ++i) {
      line += "#fff ";
    }
    const auto capped = css(line);
    TEST_CHECK(capped.size() == kMaxColorsPerLine);
    TEST_CHECK(capped.back().offset == (kMaxColorsPerLine - 1) * 5);

    std::string text;
    for (int i = 0; i < 100; ++i) {
      text += line + "\n";
    }
    const auto file = findColors(text, kindOf(Language::Css), false);
    TEST_CHECK(file.size() == kMaxColorsPerFile);
    // 62 full lines of 64, then 32 on the 63rd line.
    TEST_CHECK(file.back().offset == 62 * (line.size() + 1) + 31 * 5);

    const std::string_view lines = "a: #fff\r\nb: #000\rc: #111\n\nd: #222";
    const auto many = findColors(lines, kindOf(Language::Yaml), false);
    TEST_CHECK(many.size() == 4);
    TEST_CHECK(
        many.size() == 4 && many[0].offset == 3 && many[1].offset == 12 && many[2].offset == 20 && many[3].offset == 29
    );
    TEST_CHECK((texts(lines, many) == std::vector<std::string>{"#fff", "#000", "#111", "#222"}));
    TEST_CHECK(findColors("", kindOf(Language::Css), true).empty());
    TEST_CHECK(findLineColors("", kindOf(Language::Css), true).empty());
    TEST_CHECK(findColors("a #fff", kindOf(Language::Plain), true).empty());
  }

  void testHostileLines() {
    const auto lineOf = [](std::string_view piece, std::size_t count) {
      std::string out;
      for (std::size_t i = 0; i < count; ++i) {
        out += piece;
      }
      return out;
    };
    for (const auto& line :
         {std::string(200000, '#'), lineOf("rgb(", 50000), std::string("#") + std::string(200000, 'f'),
          lineOf("rgb(1,", 30000), lineOf("http://", 30000) + "#fff", std::string(200000, '"'), lineOf("0x", 100000),
          lineOf("red", 60000)}) {
      for (const auto language : {Language::Css, Language::EcmaScript}) {
        const auto found = findLineColors(line, kindOf(language, CommentStyle::Slash), true);
        TEST_CHECK(found.size() <= kMaxColorsPerLine);
        for (const auto& match : found) {
          TEST_CHECK(match.offset + match.length <= line.size());
        }
      }
    }
  }
} // namespace

int main() {
  testHexNotations();
  testRgb();
  testOutOfRangeClamps();
  testHsl();
  testHwb();
  testMalformedFunctions();
  testHexLiteralsInCode();
  testCodeStringsOnly();
  testLanguages();
  testHashGuards();
  testNames();
  testBounds();
  testHostileLines();
  std::println("formats color codes checks passed");
  return 0;
}
