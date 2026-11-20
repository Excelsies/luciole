#pragma once

#include <array>
#include <cstdint>
#include <unistd.h>

namespace luciole {
  // Whether the file behind `fd` starts with the Bink 2 signature, `KB2` and a revision `f` to `n`, as the player's
  // typefinder reads it. Reads four bytes at offset 0 and moves no file position.
  inline bool hasBink2Signature(int fd) {
    std::array<std::uint8_t, 4> head{};
    return ::pread(fd, head.data(), head.size(), 0) == static_cast<ssize_t>(head.size())
        && head[0] == 'K'
        && head[1] == 'B'
        && head[2] == '2'
        && head[3] >= 'f'
        && head[3] <= 'n';
  }
} // namespace luciole
