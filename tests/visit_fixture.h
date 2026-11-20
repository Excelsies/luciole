#pragma once

#include "config/visit_store.h"
#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "temp_folder.h"
#include "test_check.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace test {
  // A private temporary directory that holds one visit store.
  struct VisitFixture {
    luciole::test::TempFolder tempFolder{"luciole-visit-test"};
    std::filesystem::path root = tempFolder.path();
    std::filesystem::path store() const { return root / "visits"; }
    std::filesystem::path part(std::size_t index) const { return store() / std::format("{:02}.tsv", index); }
  };

  inline void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    TEST_CHECK(output.good());
  }

  inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  inline luciole::Visit stored(const luciole::VisitStore& store, const std::string& key) {
    const auto& part = store.parts()[luciole::VisitStore::partOf(key)];
    const auto found = part.find(key);
    return found == part.end() ? luciole::Visit{} : found->second;
  }

  inline std::size_t storedCount(const luciole::VisitStore& store) {
    std::size_t total = 0;
    for (const auto& part : store.parts()) {
      total += part.size();
    }
    return total;
  }

  // `count` distinct URIs below `base` that all belong to store part `part`.
  inline std::vector<std::string> urisInPart(std::size_t part, std::size_t count, const std::string& base) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < 1'000'000 && result.size() < count; ++index) {
      auto candidate = std::format("{}/{}", base, index);
      if (luciole::VisitStore::partOf(candidate) == part) {
        result.push_back(std::move(candidate));
      }
    }
    TEST_CHECK(result.size() == count);
    return result;
  }
} // namespace test
