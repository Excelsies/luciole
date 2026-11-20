#include "formats/json_value.h"
#include "test_check.h"

#include <array>
#include <ctime>
#include <format>
#include <string>
#include <string_view>

namespace {
  using namespace luciole::formats;

  std::string nested(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

  void testScalars() {
    TEST_CHECK(parseJson("null")->type == JsonType::Null);
    TEST_CHECK(parseJson(" true ")->boolean);
    TEST_CHECK(parseJson("true")->type == JsonType::Boolean);
    TEST_CHECK(!parseJson("false")->boolean);
    TEST_CHECK(parseJson("0")->number == 0.0);
    TEST_CHECK(parseJson("-0")->number == 0.0);
    TEST_CHECK(parseJson("42")->number == 42.0);
    TEST_CHECK(parseJson("-12.5")->number == -12.5);
    TEST_CHECK(parseJson("1.5e3")->number == 1500.0);
    TEST_CHECK(parseJson("25E-1")->number == 2.5);
    TEST_CHECK(parseJson("1e+2")->type == JsonType::Number);
    TEST_CHECK(parseJson("1e-999")->number == 0.0); // underflow reads as zero
    TEST_CHECK(parseJson("\t\r\n 7 \n")->number == 7.0);
  }

  void testStrings() {
    TEST_CHECK(parseJson(R"("plain")")->text == "plain");
    TEST_CHECK(parseJson(R"("")")->type == JsonType::String);
    TEST_CHECK(parseJson(R"("q\" b\\ s\/ \b\f\n\r\t")")->text == "q\" b\\ s/ \b\f\n\r\t");
    TEST_CHECK(parseJson(R"("caf\u00e9 \u20AC")")->text == "caf\xC3\xA9 \xE2\x82\xAC");
    TEST_CHECK(parseJson(R"("\ud83d\ude00")")->text == "\xF0\x9F\x98\x80");
    TEST_CHECK(parseJson(R"("\ud83d.")")->text == "\xEF\xBF\xBD."); // a lone surrogate becomes U+FFFD
    TEST_CHECK(parseJson(R"("\ude00")")->text == "\xEF\xBF\xBD");
    TEST_CHECK(parseJson("\"\xC3\xA9\"")->text == "\xC3\xA9");
  }

  void testContainers() {
    const auto document = parseJson(R"({"a": [1, 2, {"b": null}], "c": {}, "a": "second", "d": []})");
    TEST_CHECK(document.has_value());
    TEST_CHECK(document->type == JsonType::Object);
    TEST_CHECK(document->items.size() == 4);
    TEST_CHECK((document->keys == std::vector<std::string>{"a", "c", "a", "d"}));
    const JsonValue* a = document->find("a");
    TEST_CHECK(a != nullptr);
    TEST_CHECK(a->type == JsonType::Array); // the first member of a repeated name
    TEST_CHECK(a->items.size() == 3);
    TEST_CHECK(a->item(1)->number == 2.0);
    TEST_CHECK(a->item(3) == nullptr);
    TEST_CHECK(a->item(2)->find("b")->type == JsonType::Null);
    TEST_CHECK(a->find("b") == nullptr);      // arrays have no members
    TEST_CHECK(document->item(0) == nullptr); // objects have no positions
    TEST_CHECK(document->find("c")->type == JsonType::Object);
    TEST_CHECK(document->find("c")->items.empty());
    TEST_CHECK(document->find("d")->type == JsonType::Array);
    TEST_CHECK(document->find("missing") == nullptr);
  }

  void testMalformed() {
    for (const std::string_view text :
         {"",       "   ",           "tru",      "nul",         "01",        "1.",    ".5",        "+1",         "-",
          "1e",     "1e+",           "[1,]",     "[1 2]",       "{\"a\":}",  "{a:1}", "{\"a\" 1}", "{\"a\":1,}", "1 2",
          "\"open", "\"tab\there\"", R"("\x")",  R"("\u12G4")", R"("\u12")", "1e999", "-1e999",    "[",          "{",
          "]",      "nulls",         R"("a""b")"}) {
      const auto parsed = parseJson(text);
      TEST_CHECK(!parsed.has_value());
      TEST_CHECK(!parsed.error().empty());
    }
  }

  void testLimits() {
    TEST_CHECK(parseJson(nested(64)).has_value());
    TEST_CHECK(!parseJson(nested(65)).has_value());
    TEST_CHECK(!parseJson(nested(10'000)).has_value());
    TEST_CHECK(!parseJson(std::string(10'000, '[')).has_value());
    TEST_CHECK(!parseJson(R"({"a":{"b":{"c":1}}})", JsonLimits{.maxDepth = 2}).has_value());
    TEST_CHECK(parseJson(R"({"a":{"b":1}})", JsonLimits{.maxDepth = 2}).has_value());

    TEST_CHECK(parseJson("[1,2,3,4,5,6,7,8,9]", JsonLimits{.maxValues = 10}).has_value());
    TEST_CHECK(!parseJson("[1,2,3,4,5,6,7,8,9,10]", JsonLimits{.maxValues = 10}).has_value());
    TEST_CHECK(!parseJson(R"({"a":1,"b":2})", JsonLimits{.maxValues = 2}).has_value());

    TEST_CHECK(parseJson("[1, 2]", JsonLimits{.maxBytes = 6}).has_value());
    TEST_CHECK(!parseJson("[1, 2] ", JsonLimits{.maxBytes = 6}).has_value());
  }

  void testMegabyteString() {
    const std::string big(1 << 20, 'x');
    const auto parsed = parseJson(R"({"data":")" + big + R"("})");
    TEST_CHECK(parsed.has_value());
    TEST_CHECK(parsed->find("data")->text == big);

    const auto escapes = parseJson("\"" + std::string(1 << 20, '\\') + "\""); // 2^19 escaped backslashes
    TEST_CHECK(escapes.has_value());
    TEST_CHECK(escapes->text.size() == (1U << 19));
  }

  void testTypedAccess() {
    const auto document = parseJson(R"({"n":3,"big":9007199254740992,"neg":-1,"half":2.5,"s":"7","v":[1,2.5,-3]})");
    TEST_CHECK(document.has_value());
    TEST_CHECK(wholeNumber(document->find("n")) == 3U);
    const auto largest = parseJson("9007199254740991");
    TEST_CHECK(wholeNumber(&*largest) == 9007199254740991U);
    for (const std::string_view key : {"big", "neg", "half", "s", "v", "missing"}) {
      TEST_CHECK(!wholeNumber(document->find(key)).has_value());
    }
    TEST_CHECK((numbers<3>(document->find("v")) == std::array<double, 3>{1, 2.5, -3}));
    TEST_CHECK(!numbers<2>(document->find("v")).has_value());
    TEST_CHECK(!numbers<1>(document->find("n")).has_value());
    const auto text = parseJson(R"(["1"])");
    TEST_CHECK(!numbers<1>(&*text).has_value());

    TEST_CHECK(wholeMember(*document, "n") == 3U);
    TEST_CHECK(wholeMember(*document, "missing", 8) == 8U);
    TEST_CHECK(!wholeMember(*document, "missing").has_value());
    const auto invalid = wholeMember(*document, "half", 8);
    TEST_CHECK(!invalid.has_value());
    TEST_CHECK(invalid.error().contains("half"));

    TEST_CHECK((numbersMember<3>(*document, "v", {0, 0, 0}) == std::array<double, 3>{1, 2.5, -3}));
    TEST_CHECK((numbersMember<2>(*document, "missing", {4, 5}) == std::array<double, 2>{4, 5}));
    TEST_CHECK(!numbersMember<2>(*document, "v", {4, 5}).has_value());
    TEST_CHECK(!numbersMember<1>(*document, "n", {4}).has_value());

    TEST_CHECK(arrayItems(document->find("v")).size() == 3);
    TEST_CHECK(arrayItems(document->find("n")).empty());
    TEST_CHECK(arrayItems(nullptr).empty());
    const auto names = parseJson(R"(["KHR_a", 4, "EXT_b"])");
    TEST_CHECK(holdsAnyString(&*names, {"x", "EXT_b"}));
    TEST_CHECK(!holdsAnyString(&*names, {"KHR", "4"}));
    TEST_CHECK(!holdsAnyString(document->find("s"), {"7"})); // a string is not an array
    TEST_CHECK(!holdsAnyString(nullptr, {"KHR_a"}));
  }

  // Looking up every member of a large object stays fast: each lookup does not scan the other members.
  void testManyMembers() {
    constexpr std::size_t kMembers = 40'000;
    std::string text = "{";
    for (std::size_t k = kMembers; k > 0; --k) {
      text += std::format("\"k{}\":{},", k, k);
    }
    text += R"("k1":-1,"k40000":-1})"; // repeated names: the first member counts
    const auto document = parseJson(text);
    TEST_CHECK(document.has_value());
    const std::clock_t start = std::clock();
    bool allFound = true;
    for (std::size_t k = 1; k <= kMembers; ++k) {
      const JsonValue* value = document->find(std::format("k{}", k));
      allFound = allFound && value != nullptr && value->number == static_cast<double>(k);
    }
    const double seconds = static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
    TEST_CHECK(allFound);
    TEST_CHECK(document->find("k0") == nullptr);
    TEST_CHECK(document->find("k40001") == nullptr);
    TEST_CHECK(document->find("") == nullptr);
    TEST_CHECK(seconds < 1.0);
  }

  void testEveryTruncation() {
    const std::string full = R"({"asset":{"version":"2.0"},"list":[1,-2.5e1,true,false,null,"\u00e9\n"],"o":{}})";
    TEST_CHECK(parseJson(full).has_value());
    for (std::size_t length = 0; length < full.size(); ++length) {
      TEST_CHECK(!parseJson(std::string_view(full).substr(0, length)).has_value());
    }
  }
} // namespace

int main() {
  testScalars();
  testStrings();
  testContainers();
  testMalformed();
  testLimits();
  testMegabyteString();
  testTypedAccess();
  testManyMembers();
  testEveryTruncation();
  return 0;
}
