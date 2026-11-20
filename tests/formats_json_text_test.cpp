#include "formats/json_text.h"
#include "formats/text_tokens.h"
#include "test_check.h"

#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  void testProblems() {
    TEST_CHECK(!jsonProblem(R"({"a": [1, 2.5e3, true, null, "x"]})"));
    TEST_CHECK(jsonProblem("{'a':1}") == "unexpected character ''' at line 1, column 2");
    TEST_CHECK(jsonProblem("[1,]") == "unexpected character ']' at line 1, column 4");
    TEST_CHECK(jsonProblem("") == "unexpected end of input");
    TEST_CHECK(jsonProblem("[1]\n x") == "unexpected text after the value at line 2, column 2");
    TEST_CHECK(jsonProblem("\"a\nb\"") == "control character in a string at line 1, column 3");
    const std::string deepest = std::string(kMaxJsonDepth, '[') + std::string(kMaxJsonDepth, ']');
    TEST_CHECK(!jsonProblem(deepest));
    const std::string deeper = std::string(kMaxJsonDepth + 1, '[') + std::string(kMaxJsonDepth + 1, ']');
    TEST_CHECK(jsonProblem(deeper) == "nested deeper than 256 levels");
  }

  void testPrinted() {
    constexpr std::string_view text = R"({"a":[1,{}],"b":"x"})";
    const auto printed = printJson(text);
    TEST_CHECK(!printed.capped);
    std::vector<std::string> lines;
    lines.reserve(printed.lines.size());
    for (const auto& line : printed.lines) {
      lines.push_back(lineText(text, line));
    }
    TEST_CHECK(
        (lines == std::vector<std::string>{"{", R"(  "a": [)", "    1,", "    {}", "  ],", R"(  "b": "x")", "}"})
    );
    TEST_CHECK(printed.lines[1][1].tokenClass == TokenClass::Key);
    TEST_CHECK(printed.lines[5].back().tokenClass == TokenClass::String);
  }

  void testStartOfDocument() {
    for (const std::string_view start :
         {"", "  ", "{", R"({"a)", R"({"a")", R"({"a":)", R"({"a": [1, 2)", R"({"a": "x\)", R"({"a": "\u12)", "[-",
          "[1.", "[1e", "[1e+", "[tr", "[n", R"([{"a": {}}, )", R"({"a": 1})"}) {
      TEST_CHECK(!jsonProblem(start, JsonExtent::Start));
    }
    TEST_CHECK(jsonProblem(R"({"a": [1, 2)") == "unexpected end of input");
    TEST_CHECK(jsonProblem("[1,]", JsonExtent::Start) == "unexpected character ']' at line 1, column 4");
    TEST_CHECK(jsonProblem("[1.x", JsonExtent::Start) == "invalid number at line 1, column 2");
    TEST_CHECK(jsonProblem("[trux", JsonExtent::Start) == "unexpected character 't' at line 1, column 2");
    TEST_CHECK(jsonProblem(R"(["\x)", JsonExtent::Start) == "invalid escape in a string at line 1, column 3");
    TEST_CHECK(jsonProblem("[1] [", JsonExtent::Start) == "unexpected text after the value at line 1, column 5");
    TEST_CHECK(jsonProblem(std::string(kMaxJsonDepth + 1, '['), JsonExtent::Start) == "nested deeper than 256 levels");
  }

  std::vector<std::string> printedText(std::string_view text, const PrintedJson& printed) {
    std::vector<std::string> lines;
    lines.reserve(printed.lines.size());
    for (const auto& line : printed.lines) {
      lines.push_back(lineText(text, line));
    }
    return lines;
  }

  void testEndsEarly() {
    constexpr std::string_view cut = R"({"a":[1,{"b":"tex)";
    const auto printed = printJson(cut);
    TEST_CHECK(printed.endsEarly && !printed.capped);
    TEST_CHECK(
        (printedText(cut, printed)
         == std::vector<std::string>{"{", R"(  "a": [)", "    1,", "    {", R"(      "b": "tex)"})
    );
    TEST_CHECK(printed.lines[4][1].tokenClass == TokenClass::Key);
    TEST_CHECK(printed.lines[4].back().tokenClass == TokenClass::String);

    TEST_CHECK(printJson(R"({"a":1,)").endsEarly);
    TEST_CHECK(printJson(R"("ab\")").endsEarly);
    TEST_CHECK(printJson(R"("ab)").endsEarly);
    TEST_CHECK(!printJson(R"("ab\\")").endsEarly);
    TEST_CHECK(!printJson(R"({"a":[1,"\""]})").endsEarly);
    TEST_CHECK(!printJson("[]").endsEarly);
  }

  void testLineCap() {
    std::string many = "[";
    for (std::size_t i = 0; i < kMaxTextLines + 10; ++i) {
      many += "1,";
    }
    many += "1]";
    TEST_CHECK(!jsonProblem(many));
    const auto printed = printJson(many);
    TEST_CHECK(printed.capped && printed.lines.size() == kMaxTextLines);
  }
} // namespace

int main() {
  testProblems();
  testPrinted();
  testStartOfDocument();
  testEndsEarly();
  testLineCap();
  std::println("formats json text checks passed");
  return 0;
}
