#pragma once

#include "operations/archive_input.h"
#include "operations/archive_lzma_header.h"

#include <array>
#include <lzma.h>
#include <string_view>

namespace luciole::archive_work {
  // Decode outside libarchive to validate concatenated frames, integrity checks and trailing padding.
  // Only TAR consumes this stream. Source identity remains owned by Input.
  class XzInput {
  public:
    // XZ, LZMA-alone and lzip streams; see lzmaContainerOf.
    static bool recognizes(Input& input);
    XzInput(Input& input, Work& work);
    ~XzInput();
    XzInput(const XzInput&) = delete;
    XzInput& operator=(const XzInput&) = delete;
    std::size_t read(std::span<char> buffer);
    void finish();
    // The compression a reader shows for this stream.
    std::string_view compression() const;

  private:
    static std::optional<LzmaContainer> peek(Input& input);
    // After a lzip member: whether input follows, for which the decoder starts again.
    bool nextLzipMember();
    Input& m_input;
    Work& m_work;
    lzma_stream m_stream = LZMA_STREAM_INIT;
    std::array<char, 65536> m_encoded{};
    std::uint64_t m_decoded = 0, m_budget;
    LzmaContainer m_kind = LzmaContainer::Xz;
    bool m_eof = false, m_finished = false, m_laterMember = false;
  };
} // namespace luciole::archive_work
