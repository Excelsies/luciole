#include "media/bink2_pull_reader.h"

#include <algorithm>
#include <cstring>

namespace {
  // Small, so the scattered reads of a sound index take little: the frame index is read in order and a frame is
  // read in one pull.
  constexpr std::size_t kBlock = 4096;
} // namespace

bool luciole::media::bink2::PullReader::pull(
    std::uint64_t offset, std::size_t count, void* destination, std::size_t& got
) {
  GstBuffer* buffer = nullptr;
  const GstFlowReturn flow = gst_pad_pull_range(m_sink, offset, static_cast<guint>(count), &buffer);
  got = 0;
  if (flow != GST_FLOW_OK) {
    m_failure = flow;
    return false;
  }
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ) == FALSE) {
    gst_buffer_unref(buffer);
    m_failure = GST_FLOW_ERROR;
    return false;
  }
  got = std::min(map.size, count);
  std::memcpy(destination, map.data, got);
  gst_buffer_unmap(buffer, &map);
  gst_buffer_unref(buffer);
  return true;
}

bool luciole::media::bink2::PullReader::read(std::uint64_t offset, void* destination, std::size_t count) {
  auto* out = static_cast<std::uint8_t*>(destination);
  // Each pass copies at least one byte or ends the read, so there are at most `count` passes; a block is at least
  // one pass, so a frame takes a handful.
  while (count > 0) {
    const bool cached = m_length > 0 && offset >= m_blockStart && offset < m_blockStart + m_length;
    if (cached) {
      const auto at = static_cast<std::size_t>(offset - m_blockStart);
      const auto take = std::min(count, m_length - at);
      std::memcpy(out, m_block.data() + at, take);
      out += take;
      offset += take;
      count -= take;
      continue;
    }
    std::size_t got = 0;
    if (count >= kBlock) {
      if (!pull(offset, count, out, got) || got != count) {
        m_failure = m_failure == GST_FLOW_OK ? GST_FLOW_EOS : m_failure;
        return false;
      }
      return true;
    }
    m_block.resize(kBlock);
    m_blockStart = offset - offset % kBlock;
    m_length = 0;
    if (!pull(m_blockStart, kBlock, m_block.data(), got)) {
      return false;
    }
    m_length = got;
    if (got == 0 || offset >= m_blockStart + got) {
      m_failure = GST_FLOW_EOS;
      m_length = 0;
      return false;
    }
  }
  return true;
}
