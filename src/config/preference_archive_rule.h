#pragma once

#include "config/settings.h"
#include "fs/archive_container_types.h"

#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

// The browser.open_as_archive list: the names of the archive container types that open in the archive browser.
// Reached through preference_rules.h.
namespace luciole::preference_schema {
  template <class T> struct Rule;
  template <> struct Rule<luciole::ArchiveContainerSet> {};

  inline bool accepts(const Rule<luciole::ArchiveContainerSet>& /*rule*/, const luciole::ArchiveContainerSet& /*set*/) {
    return true;
  }

  // An array of canonical type names; a repeated name counts once.
  inline std::optional<luciole::ArchiveContainerSet>
  decode(const Rule<luciole::ArchiveContainerSet>& /*rule*/, const toml::node& node) {
    const auto* names = node.as_array();
    if (names == nullptr) {
      return std::nullopt;
    }
    luciole::ArchiveContainerSet set;
    for (const auto& item : *names) {
      const auto* name = item.as_string();
      const auto index = name != nullptr ? luciole::archiveContainerByName(name->get()) : std::nullopt;
      if (!index) {
        return std::nullopt;
      }
      set.set(*index);
    }
    return set;
  }

  inline std::string expected(const Rule<luciole::ArchiveContainerSet>& /*rule*/) {
    return "expected an array of archive type names such as 'jar' or 'epub'";
  }

  inline void encode(
      toml::table& table, std::string_view key, const Rule<luciole::ArchiveContainerSet>& /*rule*/,
      const luciole::ArchiveContainerSet& value
  ) {
    toml::array names;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value.test(index)) {
        names.push_back(std::string(luciole::kArchiveContainerTypes[index].name));
      }
    }
    table.insert(key, std::move(names));
  }
} // namespace luciole::preference_schema
