#include "imaging/animation_frames.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/user_file_save.h"
#include "imaging/animation_head.h"
#include "imaging/image_limits.h"

#include <algorithm>
#include <cmath>
#include <glycin.h>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>

namespace luciole {
  struct AnimationDecoder {
    Location file;
    int maxWidth = 1;
    int maxHeight = 1;
    std::size_t budgetBytes = 0;
    std::size_t maxFrames = 1;
    GioPtr<GlyImage> image = nullptr;
    GioPtr<GlyFrameRequest> request = nullptr;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int width = 0; // every frame is scaled to this size
    int height = 0;
    std::size_t index = 0; // next frame of the play
    std::uint32_t plays = 0;
    std::uint32_t playsRead = 0;

    std::size_t frameBytes() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U; }
  };

  struct AnimationFrames::FillResult {
    std::unique_ptr<AnimationDecoder> decoder; // empty once no more frames will come
    std::vector<FramePtr> frames;
    std::optional<std::size_t> playLength; // frames of a play that ended
    bool cut = false;                      // that play ended at an error or at the frame limit
    bool fatal = false;                    // nothing could be read
    std::string message;
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::uint32_t plays = 0;
    std::size_t frameBytes = 0;
  };
} // namespace luciole

namespace {
  using luciole::AnimationDecoder;
  using luciole::AnimationFrame;
  using luciole::GioPtr;

  constexpr std::size_t kHeadBytes = 64U * 1024U;
  constexpr std::chrono::milliseconds kShortestDelay{10};
  constexpr std::chrono::milliseconds kShortDelay{100};
  constexpr std::chrono::milliseconds kLongestDelay{10 * 60 * 1000};

  // Averages premultiplied source boxes into each output pixel and returns straight alpha. Loops are bounded by
  // the source and output sizes.
  std::vector<std::uint8_t> scaledPixels(
      std::span<const std::uint8_t> source, int sourceWidth, int sourceHeight, std::size_t stride, int width, int height
  ) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    const auto edge = [](int at, int from, int to) {
      return static_cast<int>(static_cast<std::int64_t>(at) * from / to);
    };
    std::size_t write = 0;
    for (int y = 0; y < height; ++y) {
      const int top = edge(y, sourceHeight, height);
      const int bottom = std::max(top + 1, edge(y + 1, sourceHeight, height));
      for (int x = 0; x < width; ++x) {
        const int left = edge(x, sourceWidth, width);
        const int right = std::max(left + 1, edge(x + 1, sourceWidth, width));
        std::array<std::uint64_t, 4> sum{};
        for (int row = top; row < bottom; ++row) {
          const auto* pixel =
              source.data() + static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(left) * 4U;
          for (int column = left; column < right; ++column, pixel += 4) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
              sum[channel] += pixel[channel];
            }
          }
        }
        const auto count = static_cast<std::uint64_t>(bottom - top) * static_cast<std::uint64_t>(right - left);
        const std::uint64_t alpha = (sum[3] + count / 2) / count;
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const std::uint64_t color = (sum[channel] + count / 2) / count;
          out[write + channel] = alpha == 0
              ? 0
              : static_cast<std::uint8_t>(std::min<std::uint64_t>(255, (color * 255 + alpha / 2) / alpha));
        }
        out[write + 3] = static_cast<std::uint8_t>(alpha);
        write += 4;
      }
    }
    return out;
  }

  // The first line of the error, at most 240 bytes; loaders can append whole configurations.
  std::string errorText(GError* error, std::string_view fallback) {
    constexpr std::size_t kLongest = 240;
    const luciole::GErrorPtr owned(error);
    const std::string_view text = owned != nullptr && owned->message != nullptr ? owned->message : fallback;
    std::size_t end = std::min({text.find('\n'), kLongest, text.size()});
    // Steps back over at most three continuation bytes to end on a whole character.
    while (end > 0 && end < text.size() && (static_cast<std::uint8_t>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return std::string(text.substr(0, end));
  }

  bool withinLimits(std::uint32_t width, std::uint32_t height) {
    const bool sides = std::min(width, height) > 0 && std::max(width, height) <= std::uint32_t{luciole::kMaxImageSide};
    return sides && static_cast<std::uint64_t>(width) * height <= luciole::kMaxImagePixels;
  }

  // Opens the file in a sandboxed loader for a new play. Every play keeps the first play's size.
  std::optional<std::string> openPlay(AnimationDecoder& decoder) {
    decoder.image.reset();
    const auto file = luciole::gioFile(decoder.file);
    const GioPtr<GlyLoader> loader(gly_loader_new(file.get()));
    gly_loader_set_accepted_memory_formats(loader.get(), GLY_MEMORY_SELECTION_R8G8B8A8_PREMULTIPLIED);
    GError* error = nullptr;
    GioPtr<GlyImage> image(gly_loader_load(loader.get(), &error));
    if (image == nullptr) {
      return errorText(error, "The image could not be read.");
    }
    const auto width = gly_image_get_width(image.get());
    const auto height = gly_image_get_height(image.get());
    if (!withinLimits(width, height)) {
      return "The image is too large to animate.";
    }
    if (decoder.width == 0) {
      decoder.sourceWidth = static_cast<int>(width);
      decoder.sourceHeight = static_cast<int>(height);
      std::tie(decoder.width, decoder.height) = luciole::animationFrameSize(
          decoder.sourceWidth, decoder.sourceHeight, decoder.maxWidth, decoder.maxHeight, decoder.budgetBytes
      );
    } else if (static_cast<int>(width) != decoder.sourceWidth || static_cast<int>(height) != decoder.sourceHeight) {
      return "The image changed while it played.";
    }
    decoder.request.reset(gly_frame_request_new());
    gly_frame_request_set_loop_animation(decoder.request.get(), FALSE);
    decoder.image = std::move(image);
    decoder.index = 0;
    return std::nullopt;
  }

  enum class Step : std::uint8_t { Frame, End, Cut, Error };

  struct Read {
    Step step = Step::Error;
    std::shared_ptr<const AnimationFrame> frame;
    std::string message;
  };

  Read readFrame(AnimationDecoder& decoder) {
    if (decoder.index >= decoder.maxFrames) {
      return {.step = Step::Cut, .frame = {}, .message = {}};
    }
    GError* error = nullptr;
    const GioPtr<GlyFrame> frame(gly_image_get_specific_frame(decoder.image.get(), decoder.request.get(), &error));
    if (frame == nullptr) {
      if (g_error_matches(error, GLY_LOADER_ERROR, GLY_LOADER_ERROR_NO_MORE_FRAMES)) {
        g_error_free(error);
        return {.step = Step::End, .frame = {}, .message = {}};
      }
      return {.step = Step::Error, .frame = {}, .message = errorText(error, "A frame could not be read.")};
    }
    const auto width = gly_frame_get_width(frame.get());
    const auto height = gly_frame_get_height(frame.get());
    const std::size_t stride = gly_frame_get_stride(frame.get());
    gsize size = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(g_bytes_get_data(gly_frame_get_buf_bytes(frame.get()), &size));
    const bool whole =
        withinLimits(width, height) && stride >= width * 4U && size >= stride * (height - 1U) + width * 4U;
    if (gly_frame_get_memory_format(frame.get()) != GLY_MEMORY_R8G8B8A8_PREMULTIPLIED || bytes == nullptr || !whole) {
      return {.step = Step::Error, .frame = {}, .message = "A frame is malformed."};
    }
    auto out = std::make_shared<AnimationFrame>();
    out->width = decoder.width;
    out->height = decoder.height;
    out->rgba = scaledPixels(
        {bytes, size}, static_cast<int>(width), static_cast<int>(height), stride, decoder.width, decoder.height
    );
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds(std::max<std::int64_t>(gly_frame_get_delay(frame.get()), 0))
    );
    out->delay = delay <= kShortestDelay ? kShortDelay : std::min(delay, kLongestDelay);
    out->index = decoder.index++;
    return {.step = Step::Frame, .frame = std::move(out), .message = {}};
  }
} // namespace

std::pair<int, int>
luciole::animationFrameSize(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight, std::size_t budgetBytes) {
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    return {1, 1};
  }
  maxWidth = std::clamp(maxWidth, 1, kMaxImageSide);
  maxHeight = std::clamp(maxHeight, 1, kMaxImageSide);
  const std::size_t maxPixels = std::max<std::size_t>(budgetBytes / kAnimationFramesAhead / 4U, 1);
  double scale =
      std::min({1.0, static_cast<double>(maxWidth) / sourceWidth, static_cast<double>(maxHeight) / sourceHeight});
  const double pixels = static_cast<double>(sourceWidth) * sourceHeight * scale * scale;
  if (pixels > static_cast<double>(maxPixels)) {
    scale *= std::sqrt(static_cast<double>(maxPixels) / pixels);
  }
  int width = std::clamp(static_cast<int>(std::lround(sourceWidth * scale)), 1, maxWidth);
  int height = std::clamp(static_cast<int>(std::lround(sourceHeight * scale)), 1, maxHeight);
  // Rounding up, or a side held at one pixel, can pass the cap: the longer side gives way.
  if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > maxPixels) {
    if (width >= height) {
      width = std::max(1, static_cast<int>(maxPixels / static_cast<std::size_t>(height)));
    } else {
      height = std::max(1, static_cast<int>(maxPixels / static_cast<std::size_t>(width)));
    }
  }
  return {width, height};
}

luciole::AnimationFrames::AnimationFrames(
    Location file, int maxWidth, int maxHeight, std::function<void()> changed, AnimationLimits limits
)
    : m_changed(std::move(changed)), m_limits(limits), m_lifetime(std::make_shared<int>(0)) {
  m_limits.maxFrames = std::max<std::size_t>(m_limits.maxFrames, 1);
  m_decoder = std::make_unique<AnimationDecoder>(AnimationDecoder{
      .file = std::move(file),
      .maxWidth = std::clamp(maxWidth, 1, kMaxImageSide),
      .maxHeight = std::clamp(maxHeight, 1, kMaxImageSide),
      .budgetBytes = m_limits.budgetBytes,
      .maxFrames = m_limits.maxFrames,
  });
  startFill(kAnimationFramesAhead, m_limits.budgetBytes, false);
}

luciole::AnimationFrames::~AnimationFrames() { m_token.cancel(); }

void luciole::AnimationFrames::startFill(std::size_t frames, std::size_t bytes, bool loop) {
  m_decoding = true;
  auto work = [decoder = std::move(m_decoder), frames, bytes, loop, token = m_token] mutable -> FillResult {
    FillResult result;
    const auto stop = [&](std::string message) {
      result.message = std::move(message);
      decoder.reset();
    };
    if (decoder->width == 0) {
      auto head = readUserFile(decoder->file, kHeadBytes, token);
      const auto plays = head ? animationPlaysFromHead(head->bytes) : std::optional<std::uint32_t>(0);
      if (head && !plays) {
        // Still, published with no frames and without a loader.
        decoder.reset();
        return result;
      }
      const auto failed = head ? openPlay(*decoder) : std::optional<std::string>(head.error().message);
      if (failed) {
        result.fatal = true;
        stop(*failed);
        return result;
      }
      decoder->plays = *plays;
    }
    result.sourceWidth = decoder->sourceWidth;
    result.sourceHeight = decoder->sourceHeight;
    result.plays = decoder->plays;
    result.frameBytes = decoder->frameBytes();
    std::size_t taken = 0;
    // Each pass reads a frame or ends a play that holds one, so it runs at most 2 * frames + 1 times.
    while (decoder != nullptr && result.frames.size() < frames && taken + decoder->frameBytes() <= bytes) {
      if (token.cancelled()) {
        stop("Cancelled.");
        break;
      }
      auto read = readFrame(*decoder);
      if (read.step == Step::Frame) {
        taken += read.frame->rgba.size();
        result.frames.push_back(std::move(read.frame));
        continue;
      }
      if (decoder->index == 0) {
        result.fatal = decoder->playsRead == 0;
        stop(read.message.empty() ? "The image has no frames." : read.message);
        break;
      }
      result.playLength = decoder->index;
      result.cut = read.step != Step::End;
      result.message = read.message;
      ++decoder->playsRead;
      if (!loop || (decoder->plays != 0 && decoder->playsRead >= decoder->plays)) {
        decoder.reset();
      } else if (auto failed = openPlay(*decoder)) {
        stop(*failed);
      }
    }
    result.decoder = std::move(decoder);
    return result;
  };
  runJob<FillResult>(
      m_token, std::move(work), [this, lifetime = std::weak_ptr<int>(m_lifetime)](JobResult<FillResult> result) {
        if (lifetime.expired()) {
          return;
        }
        if (!result) {
          fail(result.error().message);
          notify();
          return;
        }
        apply(std::move(*result));
      }
  );
}

void luciole::AnimationFrames::apply(FillResult result) {
  m_decoding = false;
  m_decoder = std::move(result.decoder);
  if (result.fatal) {
    fail(result.message);
    notify();
    return;
  }
  std::ranges::move(result.frames, std::back_inserter(m_frames));
  if (!result.message.empty()) {
    m_message = std::move(result.message);
  }
  if (result.playLength) {
    m_frameCount = result.playLength;
    m_truncated = m_truncated || result.cut;
  }
  if (m_state == AnimationState::Opening) {
    m_sourceWidth = result.sourceWidth;
    m_sourceHeight = result.sourceHeight;
    m_plays = result.plays;
    m_frameBytes = result.frameBytes;
    m_state = m_frames.size() >= 2 ? AnimationState::Animated : AnimationState::Still;
    if (m_state == AnimationState::Still) {
      stopDecoding();
    }
  }
  if (m_state == AnimationState::Animated && !m_streaming && m_decoder == nullptr) {
    m_resident = true;
  }
  maybeFill();
  notify();
}

// Calls a copy, so the owner may destroy this object from inside the callback.
void luciole::AnimationFrames::notify() {
  if (auto changed = m_changed) {
    changed();
  }
}

void luciole::AnimationFrames::fail(std::string message) {
  m_state = AnimationState::Failed;
  m_message = message.empty() ? "The image could not be read." : std::move(message);
  stopDecoding();
}

void luciole::AnimationFrames::stopDecoding() {
  m_decoder.reset();
  m_frames.clear();
  m_shown.reset();
  m_cursor = 0;
}

void luciole::AnimationFrames::startStreaming() {
  m_streaming = true;
  if (m_cursor > 0) {
    m_shown = m_frames[m_cursor - 1];
  }
  m_frames.erase(m_frames.begin(), m_frames.begin() + static_cast<std::ptrdiff_t>(m_cursor));
  m_cursor = 0;
}

void luciole::AnimationFrames::maybeFill() {
  if (m_decoding || m_decoder == nullptr || m_finished || m_state != AnimationState::Animated) {
    return;
  }
  const std::size_t room = m_limits.budgetBytes - std::min(heldBytes(), m_limits.budgetBytes);
  if (!m_streaming) {
    // The first play is kept whole when it fits, read a few frames per step so each step publishes soon.
    if (m_frameBytes <= room) {
      startFill(kAnimationFramesAhead, room, false);
      return;
    }
    startStreaming();
  }
  if (m_frames.size() + 1 < kAnimationFramesAhead && m_frameBytes <= room) {
    startFill(kAnimationFramesAhead - 1 - m_frames.size(), room, true);
  }
}

std::shared_ptr<const luciole::AnimationFrame> luciole::AnimationFrames::next() {
  if (m_state != AnimationState::Animated || m_finished) {
    return {};
  }
  if (!m_streaming && m_cursor >= m_frames.size() && m_resident) {
    m_cursor = 0;
  }
  if (m_cursor >= m_frames.size()) {
    if (!m_decoding && m_decoder == nullptr) {
      m_finished = true;
      stopDecoding();
    }
    maybeFill();
    return {};
  }
  auto frame = m_frames[m_cursor];
  if (frame->index == 0 && ++m_play > m_plays && m_plays != 0) {
    m_finished = true;
    stopDecoding();
    return {};
  }
  if (m_streaming) {
    m_shown = frame;
    m_frames.pop_front();
  } else {
    ++m_cursor;
  }
  maybeFill();
  return frame;
}
