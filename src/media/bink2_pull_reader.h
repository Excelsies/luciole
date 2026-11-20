#pragma once

#include <cstddef>
#include <cstdint>
#include <gst/gst.h>
#include <vector>

namespace luciole::media::bink2 {
  // Reads a file through the pull-mode sink pad of an element, in blocks, so the decoder's many small reads of the
  // header and the frame index cost one pull each block. Streaming thread only.
  class PullReader {
  public:
    explicit PullReader(GstPad* sink) : m_sink(sink) {}

    // Copies `count` bytes at `offset` into `destination`; false when the file ends first or the pad stops, in
    // which case failure() says why.
    bool read(std::uint64_t offset, void* destination, std::size_t count);
    // The flow result of the last pull that failed, or GST_FLOW_OK.
    GstFlowReturn failure() const noexcept { return m_failure; }
    void clearFailure() noexcept { m_failure = GST_FLOW_OK; }
    GstFlowReturn& failureRef() noexcept { return m_failure; }
    // Drops the block held, so the next read pulls again.
    void forget() noexcept { m_length = 0; }

  private:
    bool pull(std::uint64_t offset, std::size_t count, void* destination, std::size_t& got);

    GstPad* m_sink;
    std::vector<std::uint8_t> m_block;
    std::uint64_t m_blockStart = 0;
    std::size_t m_length = 0;
    GstFlowReturn m_failure = GST_FLOW_OK;
  };
} // namespace luciole::media::bink2
