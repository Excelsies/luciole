#pragma once

#include "config/preference_archive_rule.h"
#include "config/preference_keybind_rule.h"
#include "config/preference_names.h"
#include "config/settings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <type_traits>
#include <utility>

// How each browser.* value is spelled in TOML, which values it accepts, and how it is decoded and encoded.
namespace luciole::preference_schema {
  // How a preference is spelled in TOML and which values it accepts. Enums
  // are strings from `names`.
  template <class T> struct Rule {
    std::span<const Named<T>> names;
  };
  template <> struct Rule<bool> {};
  template <> struct Rule<std::string> {};
  template <> struct Rule<int> {
    bool (*accepts)(int);
    std::string_view expected;
  };
  template <> struct Rule<double> {
    double minimum;
    double maximum;
    std::string_view expected;
  };
  template <> struct Rule<luciole::DetailsColumnOrder> {};

  inline bool accepts(const Rule<std::string>& /*rule*/, const std::string& value) {
    return value.size() <= 4096 && !value.contains('\0') && value.find_first_of("\r\n") == std::string::npos;
  }
  inline bool accepts(const Rule<bool>& /*rule*/, bool /*value*/) { return true; }
  inline bool accepts(const Rule<int>& rule, int value) { return rule.accepts(value); }
  inline bool accepts(const Rule<double>& rule, double value) { return value >= rule.minimum && value <= rule.maximum; }
  inline bool accepts(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const luciole::DetailsColumnOrder& order) {
    return luciole::validDetailsColumnOrder(order);
  }
  template <class T>
    requires std::is_enum_v<T>
  bool accepts(const Rule<T>& rule, T value) {
    return std::ranges::contains(rule.names, value, &Named<T>::value);
  }

  inline std::optional<std::string> decode(const Rule<std::string>& /*rule*/, const toml::node& node) {
    return node.value_exact<std::string>();
  }
  inline std::string expected(const Rule<std::string>& /*rule*/) {
    return "expected a single-line command of at most 4096 bytes";
  }
  inline void
  encode(toml::table& table, std::string_view key, const Rule<std::string>& /*rule*/, const std::string& value) {
    table.insert(key, value);
  }

  // Shape only; callers check accepts() as well.
  inline std::optional<bool> decode(const Rule<bool>& /*rule*/, const toml::node& node) {
    return node.value_exact<bool>();
  }
  inline std::optional<int> decode(const Rule<int>& /*rule*/, const toml::node& node) {
    const auto number = node.value_exact<std::int64_t>();
    return number && std::in_range<int>(*number) ? std::optional<int>(static_cast<int>(*number)) : std::nullopt;
  }
  // Accepts integers too, so `1` reads as 1.0.
  inline std::optional<double> decode(const Rule<double>& /*rule*/, const toml::node& node) {
    return node.is_number() ? node.value<double>() : std::nullopt;
  }
  inline std::optional<luciole::DetailsColumnOrder>
  decode(const Rule<luciole::DetailsColumnOrder>& /*rule*/, const toml::node& node) {
    const auto* columns = node.as_array();
    luciole::DetailsColumnOrder order{};
    if (columns == nullptr || columns->size() < 3 || columns->size() > order.size()) {
      return std::nullopt;
    }
    std::array<bool, luciole::kDetailsColumnCount> seen{};
    std::size_t index = 0;
    for (const auto& item : *columns) {
      const auto* name = item.as_string();
      const auto column = name ? luciole::detailsColumnFromName(name->get()) : std::nullopt;
      if (!column || seen[static_cast<std::size_t>(*column)]) {
        return std::nullopt;
      }
      seen[static_cast<std::size_t>(*column)] = true;
      order[index++] = *column;
    }
    if (!seen[0] || !seen[1] || !seen[2]) {
      return std::nullopt;
    }
    for (const auto column : luciole::kDefaultDetailsColumnOrder) {
      if (!seen[static_cast<std::size_t>(column)]) {
        order[index++] = column;
      }
    }
    return order;
  }
  template <class T>
    requires std::is_enum_v<T>
  std::optional<T> decode(const Rule<T>& rule, const toml::node& node) {
    const auto* text = node.as_string();
    if (text == nullptr) {
      return std::nullopt;
    }
    const auto found = std::ranges::find(rule.names, std::string_view(text->get()), &Named<T>::name);
    return found != rule.names.end() ? std::optional<T>(found->value) : std::nullopt;
  }

  inline std::string expected(const Rule<bool>& /*rule*/) { return "expected a boolean"; }
  inline std::string expected(const Rule<int>& rule) { return std::string(rule.expected); }
  inline std::string expected(const Rule<double>& rule) { return std::string(rule.expected); }
  inline std::string expected(const Rule<luciole::DetailsColumnOrder>& /*rule*/) {
    return "expected unique column names including 'type', 'size', and 'modified'; optional columns may be omitted";
  }
  template <class T>
    requires std::is_enum_v<T>
  std::string expected(const Rule<T>& rule) {
    std::string text = "expected ";
    for (std::size_t index = 0; index < rule.names.size(); ++index) {
      if (index > 0) {
        text += rule.names.size() == 2 ? " or " : index + 1 == rule.names.size() ? ", or " : ", ";
      }
      text += std::format("'{}'", rule.names[index].name);
    }
    return text;
  }

  inline void encode(toml::table& table, std::string_view key, const Rule<bool>& /*rule*/, bool value) {
    table.insert(key, value);
  }
  inline void encode(toml::table& table, std::string_view key, const Rule<int>& /*rule*/, int value) {
    table.insert(key, value);
  }
  inline void encode(toml::table& table, std::string_view key, const Rule<double>& /*rule*/, double value) {
    table.insert(key, value);
  }
  inline void encode(
      toml::table& table, std::string_view key, const Rule<luciole::DetailsColumnOrder>& /*rule*/,
      const luciole::DetailsColumnOrder& order
  ) {
    toml::array columns;
    for (const auto column : order) {
      columns.push_back(luciole::detailsColumnName(column));
    }
    table.insert(key, std::move(columns));
  }
  template <class T>
    requires std::is_enum_v<T>
  void encode(toml::table& table, std::string_view key, const Rule<T>& rule, T value) {
    table.insert(key, std::ranges::find(rule.names, value, &Named<T>::value)->name);
  }
} // namespace luciole::preference_schema
