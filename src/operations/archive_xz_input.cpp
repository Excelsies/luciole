#include "operations/archive_xz_input.h"

#include "operations/archive_lzma_header.h"

#include <cstdio>
#include <string>

namespace {
  using luciole::archive_work::LzmaContainer;
  constexpr std::uint64_t DecoderMemoryLimit = 128U * 1024U * 1024U;
  std::string containerName(LzmaContainer kind) {
    return kind == LzmaContainer::Xz ? "XZ" : kind == LzmaContainer::Alone ? "LZMA" : "Lzip";
  }
  [[noreturn]] void decodeError(lzma_ret status, LzmaContainer kind) {
    using namespace luciole;
    const auto name = containerName(kind);
    if (status == LZMA_MEMLIMIT_ERROR) {
      throwFileError(FileErrorKind::Unsupported, name + " archive exceeds the 128 MiB decoder memory limit.");
    }
    if (status == LZMA_OPTIONS_ERROR || status == LZMA_UNSUPPORTED_CHECK) {
      throwFileError(FileErrorKind::Unsupported, name + " archive uses unsupported compression or integrity checks.");
    }
    if (status == LZMA_MEM_ERROR) {
      throwFileError(FileErrorKind::Other, "Could not allocate memory for the " + name + " decoder.");
    }
    throwFileError(FileErrorKind::InvalidArgument, name + " archive is corrupt or truncated.");
  }
  lzma_ret startDecoder(lzma_stream* stream, LzmaContainer kind) {
    switch (kind) {
    case LzmaContainer::Xz:
      return lzma_stream_decoder(stream, DecoderMemoryLimit, LZMA_CONCATENATED | LZMA_TELL_UNSUPPORTED_CHECK);
    case LzmaContainer::Alone:
      return lzma_alone_decoder(stream, DecoderMemoryLimit);
    case LzmaContainer::Lzip:
      return lzma_lzip_decoder(stream, DecoderMemoryLimit, LZMA_CONCATENATED);
    }
    return LZMA_PROG_ERROR;
  }
} // namespace

std::optional<luciole::archive_work::LzmaContainer> luciole::archive_work::XzInput::peek(Input& input) {
  if (input.split()) {
    return std::nullopt;
  }
  std::array<unsigned char, kLzmaHeadBytes> head{};
  const auto count = input.read({reinterpret_cast<char*>(head.data()), head.size()});
  input.seek(0, SEEK_SET);
  return lzmaContainerOf(std::span(head).first(count));
}
bool luciole::archive_work::XzInput::recognizes(Input& input) { return peek(input).has_value(); }
luciole::archive_work::XzInput::XzInput(Input& input, Work& work)
    : m_input(input), m_work(work), m_budget(decodedContainerBudget(work.limits)) {
  const auto kind = peek(input);
  if (!kind) {
    throwFileError(FileErrorKind::Other, "The input is not an XZ, LZMA or lzip stream.");
  }
  m_kind = *kind;
  const auto status = startDecoder(&m_stream, m_kind);
  if (status != LZMA_OK) {
    lzma_end(&m_stream);
    decodeError(status, m_kind);
  }
}
luciole::archive_work::XzInput::~XzInput() { lzma_end(&m_stream); }
std::size_t luciole::archive_work::XzInput::read(std::span<char> buffer) {
  m_work.check();
  if (m_finished || buffer.empty()) {
    return 0;
  }
  m_stream.next_out = reinterpret_cast<std::uint8_t*>(buffer.data());
  m_stream.avail_out = buffer.size();
  while (m_stream.avail_out != 0 && !m_finished) {
    m_work.check();
    if (m_stream.avail_in == 0 && !m_eof) {
      m_stream.avail_in = m_input.read(m_encoded);
      m_stream.next_in = reinterpret_cast<const std::uint8_t*>(m_encoded.data());
      m_eof = m_stream.avail_in == 0;
    }
    const auto before = m_stream.avail_out;
    const auto status = lzma_code(&m_stream, m_eof ? LZMA_FINISH : LZMA_RUN);
    const auto produced = before - m_stream.avail_out;
    if (produced > m_budget - m_decoded) {
      throwFileError(FileErrorKind::Unsupported, "Compressed archive exceeds its decoded-container limit.");
    }
    m_decoded += produced;
    if (status == LZMA_STREAM_END) {
      m_finished = true;
      // LZMA-alone cannot be concatenated, so anything after its end is not part of the archive.
      if (m_kind == LzmaContainer::Alone && (m_stream.avail_in != 0 || (!m_eof && m_input.read(m_encoded) != 0))) {
        throwFileError(FileErrorKind::InvalidArgument, "LZMA archive has data after its end.");
      }
    } else if (status != LZMA_OK) {
      decodeError(status, m_kind);
    }
  }
  const auto produced = buffer.size() - m_stream.avail_out;
  if (m_finished) {
    lzma_end(&m_stream);
  }
  return produced;
}
void luciole::archive_work::XzInput::finish() {
  // TAR stops at its end markers; drain the remaining XZ frames, checks and
  // padding before accepting a catalog or publishing extracted data.
  std::array<char, 65536> discard{};
  while (read(discard) != 0) {
  }
}
