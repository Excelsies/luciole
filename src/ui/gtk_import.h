#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace luciole {
  std::string_view trimSpace(std::string_view text);
  // The local file an import names: a relative path (to the importing file), an absolute path or a percent-encoded
  // `file:///` URL. nullopt for another scheme or an unusable name.
  std::optional<std::filesystem::path> importedPath(std::string_view target, const std::filesystem::path& importer);
  // The target of the value of `@import`: `url("x")`, `url('x')`, `url(x)` or `"x"`; empty for anything else.
  std::string_view importTarget(std::string_view value);
} // namespace luciole
