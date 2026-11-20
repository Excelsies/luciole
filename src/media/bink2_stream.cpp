#include "media/bink2_stream.h"

#include "media/media_player.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <system_error>
#include <utility>

namespace luciole::media::bink2 {
  // Friend of Stream, so the file functions below can fill a stream that is not yet complete.
  struct StreamOpening {
    static std::unique_ptr<Stream> make() { return std::unique_ptr<Stream>(new Stream()); }
  };
} // namespace luciole::media::bink2

namespace {
  using luciole::media::bink2::Stream;

  constexpr std::size_t kHeaderBytes = 44;
  // The header, the extra word of revision i and later and the track tables of seven tracks.
  constexpr std::size_t kHeadBytes = kHeaderBytes + 4 + 7 * 12;
  constexpr std::uint64_t kMaxFrames = 1'000'000;
  constexpr std::uint32_t kAlphaFlag = 0x00100000;
  // A sound track outside these rates is not believed.
  constexpr unsigned long kMinSoundRate = 1000;
  constexpr unsigned long kMaxSoundRate = 192000;
  constexpr std::uint64_t kMaxSoundLead = 30;
  // The sound index asks one small read per frame and track: a million frames of two tracks is the most a file may
  // claim, and the index looks at its stop function every kIndexCheck frames.
  constexpr std::uint64_t kIndexCheck = 4096;
  constexpr std::uint64_t kMaxSoundChunks = 2'000'000;

  std::uint32_t le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
  }
} // namespace

namespace luciole::media::bink2 {
  std::expected<void, std::string>
  checkContainerHead(std::span<const std::uint8_t> head, std::optional<std::uint64_t> size) {
    if (head.size() < kHeaderBytes) {
      return std::unexpected("The file is too short to be a Bink 2 video.");
    }
    if (head[0] != 'K' || head[1] != 'B' || head[2] != '2') {
      return std::unexpected("This is not a Bink 2 video.");
    }
    if (head[3] < 'f' || head[3] > 'n') {
      return std::unexpected(std::format("Bink 2 revision '{}' is not supported.", static_cast<char>(head[3])));
    }
    const auto frames = le32(&head[8]);
    const auto width = le32(&head[20]);
    const auto height = le32(&head[24]);
    const auto rateNumerator = le32(&head[28]);
    const auto rateDenominator = le32(&head[32]);
    const auto flags = le32(&head[36]);
    const auto tracks = le32(&head[40]);
    const auto side = static_cast<std::uint32_t>(luciole::kMaxMediaFrameSide);
    if (width == 0 || height == 0 || width > side || height > side) {
      return std::unexpected(std::format("The video is {} by {} pixels; at most {} are played.", width, height, side));
    }
    if (frames == 0 || frames > kMaxFrames) {
      return std::unexpected(std::format("The video claims {} frames.", frames));
    }
    // The rate becomes the caps' fraction, whose parts are signed 32 bit.
    constexpr auto kMaxRatePart = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (rateNumerator == 0 || rateDenominator == 0 || rateNumerator > kMaxRatePart || rateDenominator > kMaxRatePart) {
      return std::unexpected("The video has no usable frame rate.");
    }
    if ((flags & kAlphaFlag) != 0) {
      return std::unexpected("Bink 2 videos with a transparency plane are not supported.");
    }
    if (tracks > 7) {
      return std::unexpected(std::format("The video claims {} sound tracks.", tracks));
    }
    // The track tables follow the header, then the frame index of frames + 1 entries.
    const std::uint64_t tables = kHeaderBytes + (head[3] >= 'i' ? 4U : 0U);
    const std::uint64_t indexEnd = tables + 12ULL * tracks + 4ULL * (static_cast<std::uint64_t>(frames) + 1);
    if (size && *size < indexEnd) {
      return std::unexpected(std::format("The file is shorter than the {} frames it claims need.", frames));
    }
    if (head.size() < tables + 8ULL * tracks) {
      return size ? std::unexpected("The file is too short to be a Bink 2 video.") : std::expected<void, std::string>{};
    }
    for (std::uint32_t track = 0; track < tracks; ++track) {
      const auto largest = le32(&head[tables + 4ULL * track]);
      const auto rate = le32(&head[tables + 4ULL * tracks + 4ULL * track]) & 0xFFFFFFU;
      const unsigned channels = (head[tables + 4ULL * tracks + 4ULL * track + 3] & 0x20U) != 0 ? 2U : 1U;
      if (rate < kMinSoundRate || rate > kMaxSoundRate) {
        return std::unexpected(std::format("Sound track {} runs at {} samples a second.", track, rate));
      }
      // The sound runs ahead of the picture by what the encoder buffers, a fraction of a second (0.76 s in a real
      // file); a chunk is refused above kMaxSoundLead seconds of 16 bit samples, which also bounds the sound the
      // decoder keeps.
      if (largest == 0 || largest > static_cast<std::uint64_t>(rate) * channels * 2 * kMaxSoundLead) {
        return std::unexpected(std::format("Sound track {} claims chunks of {} bytes.", track, largest));
      }
    }
    return {};
  }
} // namespace luciole::media::bink2

namespace {
  int readThrough(void* user, unsigned long offset, void* destination, unsigned long count) {
    const auto& read = *static_cast<const Stream::Read*>(user);
    return read(offset, destination, count) ? 0 : -1;
  }
} // namespace

namespace luciole::media::bink2 {
  std::expected<std::unique_ptr<Stream>, std::string>
  Stream::open(Read read, std::uint64_t size, const std::function<bool()>& stopped) {
    std::array<std::uint8_t, kHeadBytes> head{};
    const auto headBytes = static_cast<std::size_t>(std::min<std::uint64_t>(size, head.size()));
    if (headBytes > 0 && !read(0, head.data(), headBytes)) {
      return std::unexpected("The file cannot be read.");
    }
    if (auto problem = checkContainerHead(std::span(head).first(headBytes), size); !problem) {
      return std::unexpected(std::move(problem.error()));
    }
    const std::uint32_t rateNumerator = le32(&head[28]);
    const std::uint32_t rateDenominator = le32(&head[32]);
    if (const auto chunks = static_cast<std::uint64_t>(le32(&head[8])) * le32(&head[40]); chunks > kMaxSoundChunks) {
      return std::unexpected("The video claims too many sound chunks.");
    }
    auto stream = StreamOpening::make();
    stream->m_read = std::move(read);
    stream->m_file = bink2_open_callbacks(readThrough, &stream->m_read, static_cast<unsigned long>(size));
    if (stream->m_file == nullptr) {
      return std::unexpected("The Bink 2 header or frame index is damaged.");
    }
    unsigned long frames = 0;
    unsigned long width = 0;
    unsigned long height = 0;
    double microseconds = 0;
    unsigned long flags = 0;
    (void)bink2_info_all(stream->m_file, nullptr, &frames, &microseconds);
    (void)bink2_info_video(stream->m_file, &width, &height, &flags);
    unsigned char mask = 0;
    std::array<unsigned char, 7> channels{};
    std::array<unsigned char, 7> depth{};
    std::array<unsigned long, 7> rates{};
    (void)bink2_info_audio(stream->m_file, &mask, channels.data(), depth.data(), rates.data());
    auto& info = stream->m_info;
    info.width = static_cast<int>(width);
    info.height = static_cast<int>(height);
    info.frames = frames;
    info.frameRateNumerator = rateNumerator;
    info.frameRateDenominator = rateDenominator;
    for (int track = 0; track < 7; ++track) {
      if ((mask & (1U << track)) == 0) {
        continue;
      }
      const auto index = static_cast<std::size_t>(track);
      info.audio.push_back({.rate = static_cast<int>(rates[index]), .channels = channels[index]});
      (void)bink2_enable_audio(stream->m_file, static_cast<unsigned char>(track), 1);
    }
    std::array<int, 3> stride{};
    std::array<unsigned long, 3> bytes{};
    if (bink2_frame_geometry(stream->m_file, stride.data(), bytes.data()) != 0) {
      return std::unexpected("This Bink 2 video cannot be decoded.");
    }
    for (std::size_t plane = 0; plane < 3; ++plane) {
      stream->m_layout.stride[plane] = stride[plane];
      stream->m_layout.size[plane] = bytes[plane];
      stream->m_layout.total += bytes[plane];
    }
    // At most kMaxFrames passes: the library refuses more frames than that.
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
      if (bink2_frame_is_key(stream->m_file, frame) == 1) {
        stream->m_keys.push_back(static_cast<std::uint32_t>(frame));
      }
    }
    if (stream->m_keys.empty() || stream->m_keys.front() != 0) {
      return std::unexpected("The video does not start with a key frame.");
    }
    if (auto indexed = stream->indexSound(stopped); !indexed) {
      return std::unexpected(std::move(indexed.error()));
    }
    for (auto& scratch : stream->m_scratch) {
      scratch = bink2_scratch_create(stream->m_file);
      if (scratch == nullptr) {
        return std::unexpected("Not enough memory to decode the video.");
      }
    }
    try {
      stream->m_pool = std::make_unique<SlicePool>();
    } catch (const std::system_error& error) {
      return std::unexpected(std::format("The decoder's threads could not be started: {}.", error.what()));
    }
    return stream;
  }

  std::expected<void, std::string> Stream::indexSound(const std::function<bool()>& stopped) {
    m_soundBefore.resize(m_info.audio.size());
    for (std::size_t track = 0; track < m_info.audio.size(); ++track) {
      auto& before = m_soundBefore[track];
      before.assign(1, 0);
      before.reserve(m_info.frames + 1);
      const std::uint64_t bytesPerSample = static_cast<std::uint64_t>(m_info.audio[track].channels) * 2;
      for (std::uint64_t frame = 0; frame < m_info.frames; ++frame) {
        if (frame % kIndexCheck == 0 && stopped && stopped()) {
          return std::unexpected("Opening the video was stopped.");
        }
        unsigned long bytes = 0;
        const int status = bink2_frame_sound_bytes(m_file, frame, static_cast<int>(track), &bytes);
        if (status < 0) {
          return std::unexpected(std::format("The sound of frame {} is damaged.", frame));
        }
        // A frame that reaches past the end of a cut file has no sound to count.
        before.push_back(before.back() + (status == 0 ? bytes / bytesPerSample : 0));
      }
    }
    return {};
  }

  std::uint64_t Stream::soundBefore(int track, std::uint64_t frame) const {
    const auto& before = m_soundBefore[static_cast<std::size_t>(track)];
    return before[std::min<std::uint64_t>(frame, m_info.frames)];
  }

  std::uint64_t Stream::soundFrameAt(int track, std::uint64_t samples) const {
    const auto& before = m_soundBefore[static_cast<std::size_t>(track)];
    // before[f + 1] is the count through frame f.
    const auto it = std::upper_bound(before.begin() + 1, before.end(), samples);
    return static_cast<std::uint64_t>(it - (before.begin() + 1));
  }

  std::expected<void, std::string> Stream::decodeSound(std::uint64_t frame) {
    if (frame >= m_info.frames || bink2_frame_prepare_sound(m_file, frame) != BINK2_OK) {
      return std::unexpected(std::format("The sound of frame {} is damaged.", frame));
    }
    return {};
  }

  void Stream::release() noexcept {
    // The pool's threads end before the decoder they work on.
    m_pool.reset();
    for (auto& scratch : m_scratch) {
      bink2_scratch_destroy(std::exchange(scratch, nullptr));
    }
    if (m_file != nullptr) {
      bink2_close(std::exchange(m_file, nullptr));
    }
  }

  Stream::~Stream() { release(); }

  std::expected<void, std::string> Stream::decode(std::uint64_t frame, const Planes& cur, const Planes* ref) {
    if (frame >= m_info.frames) {
      return std::unexpected("There is no such frame.");
    }
    const auto damaged = [frame] { return std::unexpected(std::format("Frame {} of the video is damaged.", frame)); };
    if (bink2_frame_prepare(m_file, frame) != BINK2_OK) {
      return damaged();
    }
    const bool key = bink2_frame_is_key(m_file, frame) == 1;
    if (!key && ref == nullptr) {
      return std::unexpected(std::format("Frame {} needs the frame before it.", frame));
    }
    Bink2Planes target{};
    Bink2Planes before{};
    for (std::size_t plane = 0; plane < 3; ++plane) {
      target.plane[plane] = cur.plane[plane];
      before.plane[plane] = ref != nullptr ? ref->plane[plane] : nullptr;
    }
    std::atomic<int> failed{0};
    m_pool->run(bink2_frame_slices(m_file), [&](int worker, int slice) {
      if (bink2_frame_decode_slice(
              m_file, m_scratch[static_cast<std::size_t>(worker)], slice, &target, key ? nullptr : &before
          )
          != 0) {
        ++failed;
      }
    });
    if (failed != 0 || bink2_frame_finish(m_file) == BINK2_ERROR) {
      return damaged();
    }
    return {};
  }

  std::span<const std::uint8_t> Stream::audio(int track) const {
    const auto index = static_cast<unsigned char>(track);
    const auto* pcm = bink2_get_audio(m_file, index);
    const auto bytes = bink2_get_audio_size(m_file, index);
    return pcm != nullptr ? std::span<const std::uint8_t>(pcm, bytes) : std::span<const std::uint8_t>{};
  }

  void Stream::resetAudio() { bink2_frame_reset_audio(m_file); }
} // namespace luciole::media::bink2
