#include "operations/archive_lzma_header.h"

#include <algorithm>
#include <array>

namespace {
  using luciole::archive_work::LzmaContainer;
  constexpr std::array<unsigned char, 6> kXzMagic{0xFD, '7', 'z', 'X', 'Z', 0};
  constexpr std::array<unsigned char, 4> kLzipMagic{'L', 'Z', 'I', 'P'};
  constexpr unsigned kPropertiesCount = 9 * 5 * 5;
  constexpr std::uint32_t kMinDictionary = 4096;
  constexpr std::uint64_t kUnknownSize = ~std::uint64_t{0};
  constexpr std::uint64_t kMaxKnownSize = std::uint64_t{1} << 38;

  std::uint64_t littleEndian(std::span<const unsigned char> bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = bytes.size(); i-- > 0;) {
      value = value << 8 | bytes[i];
    }
    return value;
  }
  bool dictionaryLooksWritten(std::uint32_t size) {
    if (size == UINT32_MAX) {
      return true;
    }
    if (size < kMinDictionary) {
      return false;
    }
    auto rounded = size - 1;
    for (const unsigned shift : {2U, 3U, 4U, 8U, 16U}) {
      rounded |= rounded >> shift;
    }
    return rounded + 1 == size;
  }
  bool aloneHeader(std::span<const unsigned char> head) {
    if (head.size() < luciole::archive_work::kLzmaHeadBytes || head[0] >= kPropertiesCount) {
      return false;
    }
    const auto size = littleEndian(head.subspan(5, 8));
    return dictionaryLooksWritten(static_cast<std::uint32_t>(littleEndian(head.subspan(1, 4))))
        && (size == kUnknownSize || size < kMaxKnownSize)
        && head[13] == 0;
  }
} // namespace

std::optional<luciole::archive_work::LzmaContainer>
luciole::archive_work::lzmaContainerOf(std::span<const unsigned char> head) {
  if (head.size() >= kXzMagic.size() && std::ranges::equal(head.first(kXzMagic.size()), kXzMagic)) {
    return LzmaContainer::Xz;
  }
  if (head.size() >= kLzipMagic.size() + 1
      && std::ranges::equal(head.first(kLzipMagic.size()), kLzipMagic)
      && head[4] <= 1) {
    return LzmaContainer::Lzip;
  }
  if (aloneHeader(head)) {
    return LzmaContainer::Alone;
  }
  return std::nullopt;
}
