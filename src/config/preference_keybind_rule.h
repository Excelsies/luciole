#pragma once

#include "config/keybinds.h"
#include "config/settings.h"

#include <optional>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

// The browser.keybinds table: command names with up to four shortcut strings each. Reached through
// preference_rules.h.
namespace luciole::preference_schema {
  template <class T> struct Rule;
  template <> struct Rule<luciole::KeybindConfig> {};

  inline bool accepts(const Rule<luciole::KeybindConfig>& /*rule*/, const luciole::KeybindConfig& config) {
    return luciole::keybindConfigValid(config);
  }

  // A table names the commands it changes; the others keep the chords of `base`.
  inline std::optional<luciole::KeybindConfig>
  decode(const Rule<luciole::KeybindConfig>& /*rule*/, const toml::node& node, const luciole::KeybindConfig& base) {
    const auto* table = node.as_table();
    if (table == nullptr) {
      return std::nullopt;
    }
    luciole::KeybindConfig config = base;
    for (const auto& [key, value] : *table) {
      const auto command = luciole::keybindCommandFromKey(key.str());
      const auto* chords = value.as_array();
      if (!command || chords == nullptr || chords->size() > luciole::kMaxKeybindChords) {
        return std::nullopt;
      }
      luciole::KeybindChords parsed{};
      std::size_t index = 0;
      for (const auto& item : *chords) {
        const auto* spec = item.as_string();
        const auto chord = spec != nullptr ? luciole::parseKeybindChord(spec->get()) : std::nullopt;
        if (!chord) {
          return std::nullopt;
        }
        parsed[index++] = *chord;
      }
      config.chords(*command) = parsed;
    }
    return luciole::keybindConfigValid(config) ? std::optional(config) : std::nullopt;
  }

  inline std::string expected(const Rule<luciole::KeybindConfig>& /*rule*/) {
    return "expected a table of command names with arrays of at most four shortcut strings";
  }

  // Writes the commands whose chords differ from `inherited`.
  inline void encode(
      toml::table& table, std::string_view key, const luciole::KeybindConfig& value,
      const luciole::KeybindConfig& inherited
  ) {
    toml::table commands;
    for (const auto scope : luciole::kKeybindScopes) {
      for (const auto command : luciole::keybindCommands(scope)) {
        if (value.chords(command) == inherited.chords(command)) {
          continue;
        }
        toml::array chords;
        for (const auto& chord : value.chords(command)) {
          if (chord) {
            chords.push_back(luciole::keybindChordSpec(*chord));
          }
        }
        commands.insert(luciole::keybindCommandKey(command), std::move(chords));
      }
    }
    if (!commands.empty()) {
      table.insert(key, std::move(commands));
    }
  }
} // namespace luciole::preference_schema
