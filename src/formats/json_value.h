#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luciole::formats {
  enum class JsonType : std::uint8_t { Null, Boolean, Number, String, Array, Object };

  struct JsonLimits {
    std::size_t maxDepth = 64;                // arrays and objects nested inside each other
    std::size_t maxValues = 1'000'000;        // values of every kind, members included
    std::size_t maxBytes = 512 * 1024 * 1024; // the whole text
  };

  // One parsed value. Only the fields of its type are set; the others keep their defaults.
  struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;                  // a string's characters as UTF-8
    std::vector<JsonValue> items;      // array elements, or object member values in file order
    std::vector<std::string> keys;     // object member names, one per item
    std::vector<std::size_t> keyOrder; // member positions sorted by name, then by position; parseJson fills it

    // The first member named `key`, or null when there is none or this is not an object. A binary search over
    // keyOrder: members it does not list are not found.
    const JsonValue* find(std::string_view key) const;
    // The array element at `index`, or null when there is none or this is not an array.
    const JsonValue* item(std::size_t index) const;
  };

  // A whole number from 0 to 2^53 - 1, which a double holds exactly; nothing for any other value or null.
  std::optional<std::size_t> wholeNumber(const JsonValue* value);

  // An array of exactly N numbers; nothing for any other value or null.
  template <std::size_t N> std::optional<std::array<double, N>> numbers(const JsonValue* value) {
    if (value == nullptr || value->type != JsonType::Array || value->items.size() != N) {
      return std::nullopt;
    }
    std::array<double, N> out{};
    for (std::size_t k = 0; k < N; ++k) {
      if (value->items[k].type != JsonType::Number) {
        return std::nullopt;
      }
      out.at(k) = value->items[k].number;
    }
    return out;
  }

  // An object member read with numbers; `fallback` when the member is absent.
  template <std::size_t N>
  std::optional<std::array<double, N>>
  numbersMember(const JsonValue& object, std::string_view key, const std::array<double, N>& fallback) {
    const JsonValue* value = object.find(key);
    return value == nullptr ? std::optional(fallback) : numbers<N>(value);
  }

  // The elements of an array; none for any other value or null.
  std::span<const JsonValue> arrayItems(const JsonValue* value);

  // Whether an array holds a string equal to one of `texts`.
  bool holdsAnyString(const JsonValue* array, std::initializer_list<std::string_view> texts);

  // An object member read with wholeNumber; `fallback` when the member is absent, an error naming it otherwise.
  std::expected<std::size_t, std::string>
  wholeMember(const JsonValue& object, std::string_view key, std::optional<std::size_t> fallback = std::nullopt);

  // Parses RFC 8259 JSON. Strings may hold any bytes but control characters; an escaped lone surrogate becomes
  // U+FFFD. A number beyond the double range is an error, one too small for it reads as zero. Nesting recurses
  // at most maxDepth levels. The error names the problem and its byte offset.
  std::expected<JsonValue, std::string> parseJson(std::string_view text, const JsonLimits& limits = {});
} // namespace luciole::formats
