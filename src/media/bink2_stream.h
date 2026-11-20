#pragma once

#include "media/bink2_slice_pool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <bink2.h>
#include <bink2_frames.h>
}

namespace luciole::media::bink2 {
  // The planes of one picture, as the decoder lays them out: Y, U, V.
  struct Planes {
    std::array<std::uint8_t*, 3> plane{};
  };

  struct AudioTrack {
    int rate = 0;
    int channels = 0;
  };

  struct Info {
    int width = 0;
    int height = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 0;
    std::uint64_t frames = 0;
    std::vector<AudioTrack> audio;
  };

  // How the decoder lays out one picture: row strides and the bytes of each padded plane.
  struct Layout {
    std::array<int, 3> stride{};
    std::array<std::size_t, 3> size{};
    std::size_t total = 0;
  };

  // Whether the start of a Bink 2 file, `head` (at least the 44 header bytes, more for the sound tables), can be
  // played: the signature, revision, picture size, frame count, rate (each part at most INT_MAX), no alpha plane, at
  // most seven tracks with believable rates and chunk sizes. With the file's `size`, the file must also hold the frame
  // index the header claims. Nothing is allocated for what it refuses.
  std::expected<void, std::string>
  checkContainerHead(std::span<const std::uint8_t> head, std::optional<std::uint64_t> size);

  // A Bink 2 file read through a function, decoded one frame at a time with the slices of a frame on a pool of
  // threads. One thread at a time.
  class Stream {
  public:
    // Reads `count` bytes at `offset` into `destination`; false when it cannot.
    using Read = std::function<bool(std::uint64_t offset, void* destination, std::size_t count)>;

    // `stopped`, when given, is asked every few thousand frames while the sound index is read, and a true ends the
    // open with an error, so a close or a state change never waits for a file that claims a million frames.
    // The stream of a file of `size` bytes, or why it cannot be played: not a Bink 2 file, a picture side past
    // kMaxMediaFrameSide, an alpha plane, a header that is cut short or contradicts itself.
    static std::expected<std::unique_ptr<Stream>, std::string>
    open(Read read, std::uint64_t size, const std::function<bool()>& stopped = {});
    ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    const Info& info() const noexcept { return m_info; }
    const Layout& layout() const noexcept { return m_layout; }
    // The frames that need no earlier frame, in order.
    const std::vector<std::uint32_t>& keyFrames() const noexcept { return m_keys; }

    // Decodes `frame` into `cur`. A key frame needs no `ref`; any other needs the planes of the frame before it. The
    // planes need layout().size bytes each. On failure, the text says what is wrong with the file; a failed read
    // is told by the Read function's own caller.
    [[nodiscard]] std::expected<void, std::string> decode(std::uint64_t frame, const Planes& cur, const Planes* ref);
    // The sound of the frame decoded last, 16 bit samples in the order the track has channels; empty when it has none.
    std::span<const std::uint8_t> audio(int track) const;
    // Forgets the sound history, so the sound after a jump starts clean.
    void resetAudio();
    // Reads and decodes only the sound of `frame`, for sound that starts before the picture it belongs to.
    [[nodiscard]] std::expected<void, std::string> decodeSound(std::uint64_t frame);
    // The samples of `track` the frames before `frame` hold, so the sound of a frame has its place in time: the
    // sound runs ahead of the pictures, the first frame of a file with sound holding a good part of a second of it.
    std::uint64_t soundBefore(int track, std::uint64_t frame) const;
    // The first frame whose sound reaches past `samples`, or the number of frames when none does.
    std::uint64_t soundFrameAt(int track, std::uint64_t samples) const;

  private:
    Stream() = default;

    ::bink2 m_file = nullptr;
    Read m_read;
    Info m_info;
    Layout m_layout;
    std::vector<std::uint32_t> m_keys;
    std::vector<std::vector<std::uint64_t>> m_soundBefore; // per track, frames + 1 counts
    std::unique_ptr<SlicePool> m_pool;
    std::array<bink2_scratch, SlicePool::kWorkers> m_scratch{};

    friend struct StreamOpening;
    std::expected<void, std::string> indexSound(const std::function<bool()>& stopped);
    void release() noexcept;
  };
} // namespace luciole::media::bink2
