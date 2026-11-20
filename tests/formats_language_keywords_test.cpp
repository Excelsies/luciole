#include "formats/file_kind.h"
#include "formats/language_keywords.h"
#include "test_check.h"

#include <algorithm>
#include <print>
#include <string_view>

namespace {
  using namespace luciole::formats;

  void testKeywords() {
    for (const auto language :
         {Language::C,          Language::Cpp,        Language::CSharp, Language::Java,       Language::Kotlin,
          Language::EcmaScript, Language::TypeScript, Language::Rust,   Language::Go,         Language::Swift,
          Language::Zig,        Language::Php,        Language::Python, Language::Ruby,       Language::Lua,
          Language::Shell,      Language::Fish,       Language::Nix,    Language::Haskell,    Language::Sql,
          Language::Meson,      Language::CMake,      Language::Make,   Language::Dockerfile, Language::Css,
          Language::Scss}) {
      const auto set = keywords(language);
      TEST_CHECK(!set.words.empty());
      TEST_CHECK(std::ranges::is_sorted(set.words));
      TEST_CHECK(std::ranges::adjacent_find(set.words) == set.words.end());
    }
    TEST_CHECK(std::ranges::binary_search(keywords(Language::Rust).words, std::string_view{"fn"}));
    TEST_CHECK(std::ranges::binary_search(keywords(Language::Cpp).words, std::string_view{"constexpr"}));
    TEST_CHECK(std::ranges::binary_search(keywords(Language::Cpp).words, std::string_view{"#include"}));
    TEST_CHECK(!std::ranges::binary_search(keywords(Language::C).words, std::string_view{"class"}));
    TEST_CHECK(std::ranges::binary_search(keywords(Language::Python).words, std::string_view{"def"}));
    TEST_CHECK(std::ranges::binary_search(keywords(Language::Dockerfile).words, std::string_view{"FROM"}));
    TEST_CHECK(keywords(Language::Sql).caseless);
    TEST_CHECK(keywords(Language::CMake).caseless);
    TEST_CHECK(!keywords(Language::C).caseless);
    for (const auto language :
         {Language::None, Language::Plain, Language::Code, Language::Json, Language::Yaml, Language::Xml,
          Language::Html, Language::Diff, static_cast<Language>(200)}) {
      TEST_CHECK(keywords(language).words.empty());
    }
  }
} // namespace

int main() {
  testKeywords();
  std::println("formats language keywords checks passed");
  return 0;
}
