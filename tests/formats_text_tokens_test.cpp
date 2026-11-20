#include "formats/text_tokens.h"
#include "test_check.h"

#include <algorithm>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace luciole::formats;
  using Classes = std::vector<std::pair<std::string, TokenClass>>;

  constexpr auto kKey = TokenClass::Key;
  constexpr auto kString = TokenClass::String;
  constexpr auto kNumber = TokenClass::Number;
  constexpr auto kLiteral = TokenClass::Literal;
  constexpr auto kComment = TokenClass::Comment;
  constexpr auto kPunct = TokenClass::Punct;
  constexpr auto kTag = TokenClass::Tag;
  constexpr auto kAttr = TokenClass::Attr;
  constexpr auto kKeyword = TokenClass::Keyword;
  constexpr auto kPlain = TokenClass::Plain;

  std::size_t codePoints(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char c) {
      return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
    }));
  }

  std::string repeat(std::string_view piece, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
      out += piece;
    }
    return out;
  }

  std::vector<std::string> plain(std::string_view text, const FormattedText& formatted) {
    std::vector<std::string> out;
    out.reserve(formatted.lines.size());
    for (const auto& line : formatted.lines) {
      out.push_back(lineText(text, line));
    }
    return out;
  }

  std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      out += (i > 0 ? "\n" : "") + lines[i];
    }
    return out;
  }

  // Tokens of one line whose text is not blank, trimmed of spaces and tabs, with their classes.
  Classes classes(std::string_view text, const FormattedText& formatted, std::size_t line) {
    Classes out;
    for (const auto& token : formatted.lines.at(line)) {
      const auto piece = tokenText(text, token);
      const auto first = piece.find_first_not_of(" \t");
      if (first != std::string::npos) {
        out.emplace_back(piece.substr(first, piece.find_last_not_of(" \t") - first + 1), token.tokenClass);
      }
    }
    return out;
  }

  Classes nonPlain(const Classes& all) {
    Classes out;
    std::ranges::copy_if(all, std::back_inserter(out), [](const auto& item) { return item.second != kPlain; });
    return out;
  }

  bool hasToken(const Classes& all, std::string_view piece, TokenClass tokenClass) {
    return std::ranges::any_of(all, [&](const auto& item) { return item.first == piece && item.second == tokenClass; });
  }

  // Every token reads bytes of the text or is one of the two byte-less classes.
  bool rangesValid(std::string_view text, const FormattedText& formatted) {
    return std::ranges::all_of(formatted.lines, [&](const TextLine& line) {
      return std::ranges::all_of(line, [&](const Token& token) {
        if (token.tokenClass == TokenClass::Space) {
          return token.length > 0 && token.length <= 2 * kMaxJsonDepth;
        }
        if (token.tokenClass == TokenClass::More) {
          return token.length == 0 && token.offset <= text.size();
        }
        return token.length > 0 && std::size_t{token.offset} + token.length <= text.size();
      });
    });
  }

  FormattedText json(std::string_view text) { return formatText(text, Language::Json); }

  void testJsonPrinted() {
    const std::string_view text = R"({"a":1,"b":[true,null,"x"],"c":{},"d":[],"e":{"f":-1.50e3}})";
    const auto r = json(text);
    TEST_CHECK(!r.error.has_value());
    TEST_CHECK(
        (plain(text, r)
         == std::vector<std::string>{
             "{", R"(  "a": 1,)", R"(  "b": [)", "    true,", "    null,", R"(    "x")", "  ],", R"(  "c": {},)",
             R"(  "d": [],)", R"(  "e": {)", R"(    "f": -1.50e3)", "  }", "}"
         })
    );
    TEST_CHECK((classes(text, r, 1) == Classes{{R"("a")", kKey}, {":", kPunct}, {"1", kNumber}, {",", kPunct}}));
    TEST_CHECK((classes(text, r, 3) == Classes{{"true", kLiteral}, {",", kPunct}}));
    TEST_CHECK((classes(text, r, 5) == Classes{{R"("x")", kString}}));
    TEST_CHECK(plain(text, r)[10] == R"(    "f": -1.50e3)");
    TEST_CHECK(r.totalLines == std::optional<std::size_t>{13});
    TEST_CHECK(!r.linesCapped);
    TEST_CHECK(rangesValid(text, r));
  }

  void testJsonStringsStayOneToken() {
    const std::string_view text = R"({"k,:{":"a\"b, c: {d}"})";
    const auto r = json(text);
    TEST_CHECK((plain(text, r) == std::vector<std::string>{"{", R"(  "k,:{": "a\"b, c: {d}")", "}"}));
    TEST_CHECK((classes(text, r, 1)[0] == std::pair<std::string, TokenClass>{R"("k,:{")", kKey}));
  }

  void testJsonScalarsAndEmpty() {
    TEST_CHECK((plain("  42 ", json("  42 ")) == std::vector<std::string>{"42"}));
    TEST_CHECK((plain(R"("s")", json(R"("s")")) == std::vector<std::string>{R"("s")"}));
    TEST_CHECK((plain("[]", json("[]")) == std::vector<std::string>{"[]"}));
    TEST_CHECK(json("").error.has_value());
  }

  void testJsonInvalidShowsRawLines() {
    const std::string_view text = "{\"a\": 1,\n  \"b\": }\n";
    const auto r = json(text);
    TEST_CHECK(r.error.has_value() && !r.error->empty());
    const auto lines = plain(text, r);
    TEST_CHECK(lines.size() >= 2);
    TEST_CHECK(lines[0] == R"({"a": 1,)");
    TEST_CHECK(lines[1] == R"(  "b": })");
    TEST_CHECK(std::ranges::all_of(r.lines, [](const TextLine& line) {
      return std::ranges::all_of(line, [](const Token& token) { return token.tokenClass == kPlain; });
    }));
    for (const auto bad :
         {std::string_view{"{'a':1}"}, std::string_view{"[1,]"}, std::string_view{"nul"},
          std::string_view{R"({"a":1} x)"}, std::string_view{"\0", 1}}) {
      TEST_CHECK(json(bad).error.has_value());
    }
  }

  void testJsonDepthLimit() {
    const auto deep = repeat("[", 300) + repeat("]", 300);
    const auto r = json(deep);
    TEST_CHECK(r.error == std::optional<std::string>{"nested deeper than 256 levels"});
    TEST_CHECK(plain(deep, r)[0] == deep.substr(0, kMaxLineWidth) + "…");
    const auto okText = repeat("[", 200) + repeat("]", 200);
    const auto ok = json(okText);
    TEST_CHECK(!ok.error.has_value());
    TEST_CHECK(ok.lines.size() == 399); // 199 opening lines, the innermost [] together, 199 closing lines
  }

  void testJsonLineCap() {
    std::string text = "[";
    for (int i = 0; i < 5000; ++i) {
      text += (i > 0 ? "," : "") + std::to_string(i);
    }
    text += ']';
    const auto r = json(text);
    TEST_CHECK(!r.error.has_value());
    TEST_CHECK(r.lines.size() == kMaxTextLines);
    TEST_CHECK(r.linesCapped);
    TEST_CHECK(!r.totalLines.has_value()); // printing stops early, so the total is unknown
  }

  std::string numberArray(int count) {
    std::string text = "[";
    for (int i = 0; i < count; ++i) {
      text += (i > 0 ? "," : "") + std::to_string(i);
    }
    return text + "]";
  }

  void testJsonLineCapEdge() {
    // "[", 1998 values and "]" fill the limit exactly.
    const auto exact = json(numberArray(1998));
    TEST_CHECK(exact.lines.size() == kMaxTextLines);
    TEST_CHECK(!exact.linesCapped);
    TEST_CHECK(exact.totalLines == std::optional<std::size_t>{kMaxTextLines});
    // One more value leaves the closing bracket out, which counts as capped.
    const auto over = json(numberArray(1999));
    TEST_CHECK(over.lines.size() == kMaxTextLines);
    TEST_CHECK(over.linesCapped);
    TEST_CHECK(!over.totalLines.has_value());
  }

  void testJsonWidthCut() {
    const auto text = R"({"k":")" + std::string(5000, 'x') + R"("})";
    const auto r = json(text);
    TEST_CHECK(r.widthCut == 1);
    const auto line = plain(text, r)[1];
    TEST_CHECK(codePoints(line) == kMaxLineWidth + 1);
    TEST_CHECK(line.ends_with("…"));
    TEST_CHECK(r.lines[1].back().tokenClass == TokenClass::More);
    TEST_CHECK(rangesValid(text, r));
  }

  void testYaml() {
    const std::string_view text =
        "# top\nname: lab # trailing\nlist:\n  - one\n  - 'two': 2\nflag: true\nurl: http://x.y/z\nq: \"a # b\"\n---\n";
    const auto r = formatText(text, Language::Yaml);
    TEST_CHECK((classes(text, r, 0) == Classes{{"# top", kComment}}));
    TEST_CHECK(
        (classes(text, r, 1) == Classes{{"name", kKey}, {":", kPunct}, {"lab", kPlain}, {"# trailing", kComment}})
    );
    TEST_CHECK((classes(text, r, 3) == Classes{{"-", kPunct}, {"one", kPlain}}));
    const auto four = classes(text, r, 4);
    TEST_CHECK(
        (Classes(four.begin(), four.begin() + 4)
         == Classes{{"-", kPunct}, {"'two'", kKey}, {":", kPunct}, {"2", kNumber}})
    );
    TEST_CHECK((classes(text, r, 5).back() == std::pair<std::string, TokenClass>{"true", kLiteral}));
    TEST_CHECK((classes(text, r, 6)[0] == std::pair<std::string, TokenClass>{"url", kKey}));
    TEST_CHECK((classes(text, r, 7).back() == std::pair<std::string, TokenClass>{R"("a # b")", kString}));
    TEST_CHECK((classes(text, r, 8) == Classes{{"---", kPunct}}));
    TEST_CHECK(joined(plain(text, r)) == text.substr(0, text.size() - 1));

    // A colon starts a value only before a space or at the end of the line.
    const std::string_view colons = "x:y\ntime 10:30\nkey:\n";
    const auto c = formatText(colons, Language::Yaml);
    TEST_CHECK((classes(colons, c, 0) == Classes{{"x:y", kPlain}}));
    TEST_CHECK(nonPlain(classes(colons, c, 1)).empty());
    TEST_CHECK((classes(colons, c, 2) == Classes{{"key", kKey}, {":", kPunct}}));

    // Unicode white space before the separator stays out of the key.
    const std::string_view spaced = "a\xC2\xA0: b\nc\xE3\x80\x80\xE2\x80\x8A: d\n\xC2\xA0: e\n";
    const auto u = formatText(spaced, Language::Yaml);
    TEST_CHECK((classes(spaced, u, 0) == Classes{{"a", kKey}, {"\xC2\xA0", kPlain}, {":", kPunct}, {"b", kPlain}}));
    TEST_CHECK(
        (classes(spaced, u, 1)
         == Classes{{"c", kKey}, {"\xE3\x80\x80\xE2\x80\x8A", kPlain}, {":", kPunct}, {"d", kPlain}})
    );
    TEST_CHECK((classes(spaced, u, 2) == Classes{{"\xC2\xA0", kPlain}, {":", kPunct}, {"e", kPlain}}));
  }

  void testToml() {
    const std::string_view text = "# c\n[server]\nhost = \"a#b\" # note\nport = 8080\nratio = 1.5e3\non = true\nwhen = "
                                  "2024-01-02\n[[items]]\nbody = \"\"\"\nline # not a comment\n\"\"\"\nafter = 1\n";
    const auto r = formatText(text, Language::Toml);
    TEST_CHECK((classes(text, r, 0) == Classes{{"# c", kComment}}));
    TEST_CHECK((classes(text, r, 1) == Classes{{"[server]", kTag}}));
    TEST_CHECK(
        (classes(text, r, 2) == Classes{{"host", kKey}, {"=", kPunct}, {R"("a#b")", kString}, {"# note", kComment}})
    );
    TEST_CHECK((classes(text, r, 3).back() == std::pair<std::string, TokenClass>{"8080", kNumber}));
    TEST_CHECK((classes(text, r, 5).back() == std::pair<std::string, TokenClass>{"true", kLiteral}));
    TEST_CHECK((classes(text, r, 6).back() == std::pair<std::string, TokenClass>{"2024-01-02", kNumber}));
    TEST_CHECK((classes(text, r, 7) == Classes{{"[[items]]", kTag}}));
    TEST_CHECK(classes(text, r, 9)[0].second == kString); // inside a multi-line string
    TEST_CHECK((classes(text, r, 11)[0] == std::pair<std::string, TokenClass>{"after", kKey}));
    TEST_CHECK(joined(plain(text, r)) + "\n" == text);
  }

  void testNumbersAndDates() {
    const std::string_view text = "a = 2024-01-02T10:20:30.5Z\nb = 2024-01-02T\nc = 2024-01-02X\nd = 2024-01-02T10Zx\n"
                                  "e = 1.\nf = 0x\ng = 1e\nh = -1_000.5e-3\ni = 0xDEAD_beef\nj = 1.5.3\nk = +7\n"
                                  "l = 1e+\nm = 2024-1-02\nx # = 1\n";
    const auto r = formatText(text, Language::Toml);
    const auto value = [&](std::size_t line) { return classes(text, r, line).back(); };
    TEST_CHECK((value(0) == std::pair<std::string, TokenClass>{"2024-01-02T10:20:30.5Z", kNumber}));
    for (const std::size_t line : {1U, 2U, 3U, 4U, 5U, 6U, 9U, 11U, 12U}) {
      TEST_CHECK(value(line).second == kPlain);
    }
    for (const std::size_t line : {7U, 8U, 10U}) {
      TEST_CHECK(value(line).second == kNumber);
    }
    // A '#' before any '=' makes the rest a comment, not a key.
    TEST_CHECK((classes(text, r, 13) == Classes{{"x", kPlain}, {"# = 1", kComment}}));
  }

  void testIni() {
    const std::string_view text = "; semi\n# hash\n[core]\nname = lab\nother: 3\nbare\n";
    const auto r = formatText(text, Language::Ini, CommentStyle::Semi);
    TEST_CHECK(classes(text, r, 0)[0].second == kComment);
    TEST_CHECK(classes(text, r, 1)[0].second == kComment);
    TEST_CHECK((classes(text, r, 2) == Classes{{"[core]", kTag}}));
    TEST_CHECK((classes(text, r, 3) == Classes{{"name", kKey}, {"=", kPunct}, {"lab", kPlain}}));
    TEST_CHECK((classes(text, r, 4) == Classes{{"other", kKey}, {":", kPunct}, {"3", kNumber}}));
    TEST_CHECK((classes(text, r, 5) == Classes{{"bare", kPlain}}));
  }

  void testXml() {
    const std::string_view text =
        "<?xml version=\"1.0\"?>\n<a x=\"1\" y='two'>\n  text &amp; more\n  <!-- c1\n  c2 -->\n  <b/>\n</a>\n";
    const auto r = formatText(text, Language::Xml);
    TEST_CHECK(r.lines[0][0].tokenClass == kComment);
    TEST_CHECK(
        (classes(text, r, 1)
         == Classes{
             {"<", kPunct},
             {"a", kTag},
             {"x", kAttr},
             {"=", kPunct},
             {R"("1")", kString},
             {"y", kAttr},
             {"=", kPunct},
             {"'two'", kString},
             {">", kPunct}
         })
    );
    TEST_CHECK((classes(text, r, 2) == Classes{{"text &amp; more", kPlain}}));
    TEST_CHECK(classes(text, r, 3)[0].second == kComment);
    TEST_CHECK(classes(text, r, 4)[0].second == kComment);
    TEST_CHECK((classes(text, r, 5) == Classes{{"<", kPunct}, {"b", kTag}, {"/>", kPunct}}));
    TEST_CHECK(joined(plain(text, r)) + "\n" == text);

    // Character data is plain text, across lines, until "]]>".
    const std::string_view cdata = "<x><![CDATA[ <a> \n & ]]><y/>\n";
    const auto d = formatText(cdata, Language::Xml);
    TEST_CHECK((nonPlain(classes(cdata, d, 0)) == Classes{{"<", kPunct}, {"x", kTag}, {">", kPunct}}));
    TEST_CHECK((classes(cdata, d, 1) == Classes{{"& ]]>", kPlain}, {"<", kPunct}, {"y", kTag}, {"/>", kPunct}}));
  }

  void testCodeSlash() {
    const std::string_view text =
        "int x = 0x1F; // note\n/* a\n b */ char *s = \"q\\\"//z\";\n#include <a.h>\nf(1.5e3, 'c');\n";
    const auto r = formatText(text, Language::Code, CommentStyle::Slash);
    TEST_CHECK((nonPlain(classes(text, r, 0)) == Classes{{"0x1F", kNumber}, {"// note", kComment}}));
    TEST_CHECK(r.lines[1][0].tokenClass == kComment);
    TEST_CHECK(classes(text, r, 2)[0].second == kComment);
    TEST_CHECK(hasToken(classes(text, r, 2), R"("q\"//z")", kString));
    TEST_CHECK(nonPlain(classes(text, r, 3)).empty());
    TEST_CHECK(hasToken(classes(text, r, 4), "1.5e3", kNumber));
    TEST_CHECK(hasToken(classes(text, r, 4), "'c'", kString));
    TEST_CHECK(joined(plain(text, r)) + "\n" == text);
  }

  void testCodeHashAndDash() {
    const std::string_view hashText = "x = 1 # c\n# whole\n";
    const auto h = formatText(hashText, Language::Code, CommentStyle::Hash);
    TEST_CHECK((classes(hashText, h, 0).back() == std::pair<std::string, TokenClass>{"# c", kComment}));
    TEST_CHECK(classes(hashText, h, 1)[0].second == kComment);
    const std::string_view dashText = "select 1 -- c\n";
    const auto d = formatText(dashText, Language::Code, CommentStyle::Dash);
    TEST_CHECK((classes(dashText, d, 0).back() == std::pair<std::string, TokenClass>{"-- c", kComment}));
    // A hash comment needs the line start or a space before it.
    const std::string_view inWord = "a=#b # c\n";
    const auto w = formatText(inWord, Language::Code, CommentStyle::Hash);
    TEST_CHECK((classes(inWord, w, 0) == Classes{{"a=#b", kPlain}, {"# c", kComment}}));
    const std::string_view slashText = "a // b\n";
    TEST_CHECK(nonPlain(classes(slashText, formatText(slashText, Language::Code, CommentStyle::Hash), 0)).empty());
  }

  void testPlainAndUnknownLanguage() {
    const std::string_view text = "a\r\nb\rc\n\nd";
    const auto r = formatText(text, Language::Plain);
    TEST_CHECK((plain(text, r) == std::vector<std::string>{"a", "b", "c", "", "d"}));
    TEST_CHECK(r.totalLines == std::optional<std::size_t>{5});
    for (const auto language : {Language::None, static_cast<Language>(200)}) {
      const auto unknown = formatText("x", language);
      TEST_CHECK(unknown.error == std::optional<std::string>{"unknown language"});
      TEST_CHECK(unknown.lines.empty());
      TEST_CHECK(unknown.totalLines == std::optional<std::size_t>{0});
    }
  }

  void testLineCap() {
    const auto text = repeat("x\n", kMaxTextLines + 500);
    const auto r = formatText(text, Language::Plain);
    TEST_CHECK(r.lines.size() == kMaxTextLines);
    TEST_CHECK(r.linesCapped);
    TEST_CHECK(r.totalLines == std::optional<std::size_t>{kMaxTextLines + 500});
  }

  void testHostileFamilies() {
    const std::vector<std::string> sources{
        std::string(200000, '"'), std::string(100000, '<'), std::string(100000, '['), repeat("/*", 50000),
        repeat("'\\", 50000)
    };
    for (const auto language : {Language::Yaml, Language::Toml, Language::Ini, Language::Xml, Language::Code}) {
      for (const auto& text : sources) {
        const auto r = formatText(text, language, CommentStyle::Slash);
        TEST_CHECK(r.lines.size() <= kMaxTextLines);
        TEST_CHECK(std::ranges::all_of(plain(text, r), [](const std::string& line) {
          return codePoints(line) <= kMaxLineWidth + 1;
        }));
        TEST_CHECK(rangesValid(text, r));
      }
    }
  }

  // Printed JSON uses exactly the printer's six classes.
  void testKnownClasses() {
    const std::string_view text = R"({"a":[1,"b",null]})";
    const auto r = json(text);
    std::vector<TokenClass> seen;
    for (const auto& line : r.lines) {
      for (const auto& token : line) {
        if (!std::ranges::contains(seen, token.tokenClass)) {
          seen.push_back(token.tokenClass);
        }
      }
    }
    std::ranges::sort(seen);
    TEST_CHECK((seen == std::vector<TokenClass>{kKey, kString, kNumber, kLiteral, kPunct, TokenClass::Space}));
    TEST_CHECK(rangesValid(text, r));
  }

  void testJsonErrorMessages() {
    TEST_CHECK(json("").error == std::optional<std::string>{"unexpected end of input"});
    TEST_CHECK(json("  \n ").error == std::optional<std::string>{"unexpected end of input"});
    TEST_CHECK(
        json("{\"a\": 1,\n  \"b\": }\n").error
        == std::optional<std::string>{"unexpected character '}' at line 2, column 8"}
    );
    TEST_CHECK(
        json(std::string_view{"\0", 1}).error == std::optional<std::string>{"unexpected byte 0x00 at line 1, column 1"}
    );
    TEST_CHECK(
        json(R"({"a":1} x)").error == std::optional<std::string>{"unexpected text after the value at line 1, column 9"}
    );
    TEST_CHECK(
        json("\"a\x01\"").error == std::optional<std::string>{"control character in a string at line 1, column 3"}
    );
    TEST_CHECK(json(R"("\x")").error == std::optional<std::string>{"invalid escape in a string at line 1, column 2"});
    TEST_CHECK(
        json(R"("\u12G4")").error == std::optional<std::string>{"invalid escape in a string at line 1, column 2"}
    );
    TEST_CHECK(json(R"("abc)").error == std::optional<std::string>{"unterminated string at line 1, column 1"});
    TEST_CHECK(json(R"("abc\)").error == std::optional<std::string>{"unterminated string at line 1, column 1"});
    TEST_CHECK(
        json(R"({"a\x":1})").error == std::optional<std::string>{"invalid escape in a string at line 1, column 4"}
    );
    TEST_CHECK(json("01").error == std::optional<std::string>{"unexpected text after the value at line 1, column 2"});
    TEST_CHECK(json("-").error == std::optional<std::string>{"invalid number at line 1, column 1"});
    TEST_CHECK(json("[1.e5]").error == std::optional<std::string>{"invalid number at line 1, column 2"});
    TEST_CHECK(json("[1e+]").error == std::optional<std::string>{"invalid number at line 1, column 2"});
    TEST_CHECK(json("{\"a\" 1}").error == std::optional<std::string>{"unexpected character '1' at line 1, column 6"});
    TEST_CHECK(json("{1:2}").error == std::optional<std::string>{"unexpected character '1' at line 1, column 2"});
    TEST_CHECK(json("[1 2]").error == std::optional<std::string>{"unexpected character '2' at line 1, column 4"});
    TEST_CHECK(json("[1}").error == std::optional<std::string>{"unexpected character '}' at line 1, column 3"});
    TEST_CHECK(
        json("truex").error == std::optional<std::string>{"unexpected text after the value at line 1, column 5"}
    );
    // Columns count code points, and CRLF or CR end a line once.
    TEST_CHECK(
        json("[\"\xC3\xA9\", x]").error == std::optional<std::string>{"unexpected character 'x' at line 1, column 7"}
    );
    TEST_CHECK(
        json("[\r\n1,\r2,\r\n x]").error == std::optional<std::string>{"unexpected character 'x' at line 4, column 2"}
    );
    // A byte order mark is not JSON whitespace.
    TEST_CHECK(json("\xEF\xBB\xBF{}").error == std::optional<std::string>{"unexpected byte 0xEF at line 1, column 1"});
    // A syntax error wins over the depth limit.
    TEST_CHECK(
        json(repeat("[", 300) + "x").error
        == std::optional<std::string>{"unexpected character 'x' at line 1, column 301"}
    );
    // Escapes that are valid.
    TEST_CHECK(!json(R"(["\"\\\/\b\f\n\r\té\uD83D"])").error.has_value());
  }

  void testJsonDepthBoundary() {
    TEST_CHECK(!json(repeat("[", 256) + repeat("]", 256)).error.has_value());
    TEST_CHECK(json(repeat("{\"a\":", 257) + "1" + repeat("}", 257)).error.has_value());
    // At depth 255 the indentation alone fills the width, so only the marker follows it.
    const auto text = repeat("[", 256) + repeat("]", 256);
    const auto r = json(text);
    TEST_CHECK(plain(text, r)[255] == std::string(kMaxLineWidth, ' ') + "…");
    TEST_CHECK(r.widthCut > 0);
    TEST_CHECK(rangesValid(text, r));
  }

  void testJsonKeepsOrderAndNumberText() {
    const std::string_view text = "{\"z\":1,\r\n\"a\":1.0e+2 , \"m\" : [ ] ,\"o\":{\n}}";
    const auto r = json(text);
    TEST_CHECK(
        (plain(text, r)
         == std::vector<std::string>{"{", R"(  "z": 1,)", R"(  "a": 1.0e+2,)", R"(  "m": [],)", R"(  "o": {})", "}"})
    );
    TEST_CHECK(rangesValid(text, r));
  }

  void testKeywords() {
    const std::string_view c = "#include <stdio.h>\nint main(void) { return 0; } /* x */\nint returned = if_x;\n";
    const auto r = formatText(c, Language::C, CommentStyle::Slash);
    TEST_CHECK((nonPlain(classes(c, r, 0)) == Classes{{"#include", kKeyword}}));
    TEST_CHECK(
        (nonPlain(classes(c, r, 1))
         == Classes{{"int", kKeyword}, {"void", kKeyword}, {"return", kKeyword}, {"0", kNumber}, {"/* x */", kComment}})
    );
    TEST_CHECK((nonPlain(classes(c, r, 2)) == Classes{{"int", kKeyword}}));
    TEST_CHECK(joined(plain(c, r)) + "\n" == c);

    const std::string_view py = "def f(x):\n    return None\nelse:\n";
    const auto p = formatText(py, Language::Python, CommentStyle::Hash);
    TEST_CHECK((nonPlain(classes(py, p, 0)) == Classes{{"def", kKeyword}}));
    TEST_CHECK((nonPlain(classes(py, p, 1)) == Classes{{"return", kKeyword}, {"None", kLiteral}}));
    TEST_CHECK((classes(py, p, 2) == Classes{{"else", kKeyword}, {":", kPlain}}));

    const std::string_view sql = "SELECT id FROM t where x = 1; -- note\n";
    const auto s = formatText(sql, Language::Sql, CommentStyle::Dash);
    TEST_CHECK((
        nonPlain(classes(sql, s, 0))
        == Classes{{"SELECT", kKeyword}, {"FROM", kKeyword}, {"where", kKeyword}, {"1", kNumber}, {"-- note", kComment}}
    ));

    const std::string_view docker = "FROM alpine AS build\nRUN from user\n";
    const auto d = formatText(docker, Language::Dockerfile, CommentStyle::Hash);
    TEST_CHECK((nonPlain(classes(docker, d, 0)) == Classes{{"FROM", kKeyword}, {"AS", kKeyword}}));
    TEST_CHECK((nonPlain(classes(docker, d, 1)) == Classes{{"RUN", kKeyword}}));

    const std::string_view css = "@media screen { a { color: red !important; } }\n";
    const auto k = formatText(css, Language::Css, CommentStyle::Slash);
    TEST_CHECK((nonPlain(classes(css, k, 0)) == Classes{{"@media", kKeyword}, {"!important", kKeyword}}));

    const std::string_view rust = "pub fn main() -> i32 { let mut x = 1_000; }\n";
    const auto rs = formatText(rust, Language::Rust, CommentStyle::Slash);
    TEST_CHECK(
        (nonPlain(classes(rust, rs, 0))
         == Classes{{"pub", kKeyword}, {"fn", kKeyword}, {"let", kKeyword}, {"mut", kKeyword}, {"1_000", kNumber}})
    );

    // Code without a language of its own has no keywords; words too long for any keyword stay plain.
    const std::string_view generic = "int x\n";
    TEST_CHECK(nonPlain(classes(generic, formatText(generic, Language::Code, CommentStyle::Slash), 0)).empty());
    const auto longWord = std::string(40, 'S') + " select";
    const auto lw = formatText(longWord, Language::Sql, CommentStyle::Dash);
    TEST_CHECK((nonPlain(classes(longWord, lw, 0)) == Classes{{"select", kKeyword}}));
  }

  void testJsonLines() {
    const std::string_view text = "{\"a\": 1, \"b\": [true, \"x\"]}\n{\"c\":null}\nnot json at all\n";
    const auto r = formatText(text, Language::JsonLines);
    TEST_CHECK(!r.error.has_value());
    TEST_CHECK(
        (classes(text, r, 0)
         == Classes{
             {"{", kPunct},
             {R"("a")", kKey},
             {":", kPunct},
             {"1", kNumber},
             {",", kPunct},
             {R"("b")", kKey},
             {":", kPunct},
             {"[", kPunct},
             {"true", kLiteral},
             {",", kPunct},
             {R"("x")", kString},
             {"]}", kPunct}
         })
    );
    TEST_CHECK(
        (classes(text, r, 1)
         == Classes{{"{", kPunct}, {R"("c")", kKey}, {":", kPunct}, {"null", kLiteral}, {"}", kPunct}})
    );
    TEST_CHECK(joined(plain(text, r)) + "\n" == text);
    TEST_CHECK(rangesValid(text, r));
  }

  void testDiff() {
    const std::string_view text = "diff --git a/x b/x\n"
                                  "index 1..2 100644\n"
                                  "--- a/x\n"
                                  "+++ b/x\n"
                                  "@@ -1,3 +1,3 @@ fn\n"
                                  " keep\n"
                                  "--- removed dashes\n"
                                  "+++ added pluses\n"
                                  "\n"
                                  "\\ No newline at end of file\n"
                                  "+outside\n"
                                  "-outside\n"
                                  "plain\n"
                                  "@@ -1 +1 @@\n"
                                  "-a\n"
                                  "+b\n"
                                  "--- next/file\n"
                                  "@@ broken @@\n"
                                  "+++ b/y\n"
                                  "@@ -1,99999999999 +1 @@\n"
                                  "--- c/z\n";
    const auto r = formatText(text, Language::Diff);
    const std::vector<TokenClass> expected{
        TokenClass::Header,
        TokenClass::Header,
        TokenClass::Header,
        TokenClass::Header,
        TokenClass::Hunk,
        kPlain,
        TokenClass::Removed,
        TokenClass::Added,
        kPlain,
        kComment,
        TokenClass::Added,
        TokenClass::Removed,
        kPlain,
        TokenClass::Hunk,
        TokenClass::Removed,
        TokenClass::Added,
        TokenClass::Header,
        TokenClass::Hunk,
        TokenClass::Header,
        TokenClass::Hunk,
        TokenClass::Header,
    };
    TEST_CHECK(r.lines.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < r.lines.size(); ++i) {
      if (i == 8) {
        TEST_CHECK(r.lines[i].empty()); // an empty context line
        continue;
      }
      TEST_CHECK(r.lines[i].size() == 1);
      TEST_CHECK(r.lines[i][0].tokenClass == expected[i]);
    }
    TEST_CHECK(joined(plain(text, r)) + "\n" == text);

    // Inside a hunk a backslash line counts nothing, and any other line ends the hunk early.
    const std::string_view early = "@@ -1,2 +1,2 @@\n\\ No newline at end of file\n x\nstray\n+y\n--- a\n";
    const auto e = formatText(early, Language::Diff);
    const std::vector<TokenClass> earlyClasses{TokenClass::Hunk, kComment,          kPlain,
                                               kPlain,           TokenClass::Added, TokenClass::Header};
    TEST_CHECK(e.lines.size() == earlyClasses.size());
    for (std::size_t i = 0; i < earlyClasses.size() && i < e.lines.size(); ++i) {
      TEST_CHECK(e.lines[i].size() == 1 && e.lines[i][0].tokenClass == earlyClasses[i]);
    }
    // Headers with missing counts are hunks without line tracking.
    for (const auto* header : {"@@ -1, +1 @@\n--- a\n", "@@ -1,2 +x @@\n--- a\n", "@@ -1,2 +1,2\n--- a\n"}) {
      const std::string_view bad = header;
      const auto b = formatText(bad, Language::Diff);
      TEST_CHECK(b.lines.size() == 2);
      TEST_CHECK(b.lines[0][0].tokenClass == TokenClass::Hunk);
      TEST_CHECK(b.lines[1][0].tokenClass == TokenClass::Header);
    }
  }

  void testDegenerateInputs() {
    for (const auto language :
         {Language::Plain, Language::Markdown, Language::Csv, Language::JsonLines, Language::Yaml, Language::Toml,
          Language::Ini, Language::Xml, Language::Code, Language::Diff, Language::Cpp}) {
      const auto empty = formatText("", language);
      TEST_CHECK(empty.lines.size() == 1);
      TEST_CHECK(empty.lines[0].empty());
      TEST_CHECK(empty.totalLines == std::optional<std::size_t>{1});
      TEST_CHECK(!empty.error.has_value());
      const auto newline = formatText("\n", language);
      TEST_CHECK(newline.lines.size() == 1);
      TEST_CHECK(newline.totalLines == std::optional<std::size_t>{1});
    }
    // Markdown and delimited text tokenize as plain lines.
    const std::string_view markdown = "# Title\n*a*, b\n";
    const auto md = formatText(markdown, Language::Markdown);
    TEST_CHECK((classes(markdown, md, 0) == Classes{{"# Title", kPlain}}));
    // CRLF never leaves a carriage return in a token.
    const std::string_view crlf = "a: 1\r\nb: 2\r\n";
    const auto y = formatText(crlf, Language::Yaml);
    TEST_CHECK((plain(crlf, y) == std::vector<std::string>{"a: 1", "b: 2"}));
    // Width counts code points: 401 two-byte characters keep 400 and the marker.
    const auto wide = repeat("\xC3\xA9", 401);
    const auto w = formatText(wide, Language::Plain);
    TEST_CHECK(plain(wide, w)[0] == repeat("\xC3\xA9", 400) + "…");
    TEST_CHECK(w.lines[0][0].length == 800);
    TEST_CHECK(w.widthCut == 1);
    const auto exact = repeat("\xC3\xA9", 400);
    TEST_CHECK(formatText(exact, Language::Plain).widthCut == 0);
    // When the limit falls between two tokens, the next token is left out whole.
    const auto boundary = std::string(400, 'x') + "\"s\"";
    const auto b = formatText(boundary, Language::Code, CommentStyle::Slash);
    TEST_CHECK(plain(boundary, b)[0] == std::string(400, 'x') + "…");
    TEST_CHECK(b.lines[0].size() == 2);
  }

  // A stray continuation byte counts as one code point, so invalid UTF-8 cannot carry a line past the width limit.
  void testInvalidUtf8Width() {
    const auto text = "a" + std::string(5'000'000, '\x80');
    for (const auto language : {Language::Plain, Language::Code, Language::Yaml}) {
      const auto r = formatText(text, language, CommentStyle::Slash);
      TEST_CHECK(r.lines.size() == 1);
      TEST_CHECK(r.widthCut == 1);
      TEST_CHECK(!r.lines[0].empty() && r.lines[0].back().tokenClass == TokenClass::More);
      TEST_CHECK(plain(text, r)[0] == "a" + std::string(kMaxLineWidth - 1, '\x80') + "…");
      TEST_CHECK(rangesValid(text, r));
    }
    const auto jsonText = R"({"k":")" + std::string(1'000'000, '\xBF') + R"("})";
    const auto j = json(jsonText);
    TEST_CHECK(!j.error.has_value());
    TEST_CHECK(j.widthCut == 1);
    TEST_CHECK(plain(jsonText, j)[1] == R"(  "k": ")" + std::string(kMaxLineWidth - 8, '\xBF') + "…");
    TEST_CHECK(rangesValid(jsonText, j));
    // A lead byte takes only the continuation bytes it announces: E0 takes two, so the third starts a code point.
    const auto overlong = repeat("\xE0\x80\x80\x80", 401);
    const auto o = formatText(overlong, Language::Plain);
    TEST_CHECK(plain(overlong, o)[0] == repeat("\xE0\x80\x80\x80", 200) + "…");
    TEST_CHECK(o.widthCut == 1);
  }

  void testTokenText() {
    const std::string_view text = "abc";
    TEST_CHECK(tokenText(text, Token{.offset = 1, .length = 2, .tokenClass = kPlain}) == "bc");
    TEST_CHECK(tokenText(text, Token{.offset = 2, .length = 9, .tokenClass = kPlain}) == "c");
    TEST_CHECK(tokenText(text, Token{.offset = 9, .length = 1, .tokenClass = kPlain}).empty());
    TEST_CHECK(tokenText(text, Token{.offset = 0, .length = 3, .tokenClass = TokenClass::Space}) == "   ");
    TEST_CHECK(
        tokenText(text, Token{.offset = 0, .length = 4000000000U, .tokenClass = TokenClass::Space}).size()
        == 2 * kMaxJsonDepth
    );
    TEST_CHECK(tokenText(text, Token{.offset = 3, .length = 0, .tokenClass = TokenClass::More}) == "…");
    TEST_CHECK(lineText(text, TextLine{}).empty());
  }
} // namespace

int main() {
  testJsonPrinted();
  testJsonStringsStayOneToken();
  testJsonScalarsAndEmpty();
  testJsonInvalidShowsRawLines();
  testJsonDepthLimit();
  testJsonLineCap();
  testJsonLineCapEdge();
  testJsonWidthCut();
  testYaml();
  testToml();
  testNumbersAndDates();
  testIni();
  testXml();
  testCodeSlash();
  testCodeHashAndDash();
  testPlainAndUnknownLanguage();
  testLineCap();
  testHostileFamilies();
  testKnownClasses();
  testJsonErrorMessages();
  testJsonDepthBoundary();
  testJsonKeepsOrderAndNumberText();
  testKeywords();
  testJsonLines();
  testDiff();
  testDegenerateInputs();
  testInvalidUtf8Width();
  testTokenText();
  std::println("formats text tokens checks passed");
  return 0;
}
