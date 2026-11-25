#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace luciole::archive_work {
  // The liblzma container formats a TAR can arrive in.
  enum class LzmaContainer : std::uint8_t { Xz, Alone, Lzip };
  // The bytes a file's start must provide to tell the containers apart.
  inline constexpr std::size_t kLzmaHeadBytes = 14;
  // The container the first bytes of a file declare, if any. XZ and lzip have magic numbers. LZMA-alone has none, so
  // it is accepted only when every header field is one liblzma itself would write: a valid properties byte, a
  // dictionary size of the form 2^n or 2^n + 2^(n-1) from 4 KiB up, an uncompressed size that is unknown or below
  // 256 GiB, and the zero byte every range-coded stream starts with.
  std::optional<LzmaContainer> lzmaContainerOf(std::span<const unsigned char> head);
} // namespace luciole::archive_work
