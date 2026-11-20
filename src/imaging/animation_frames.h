#pragma once

#include "fs/cancel_token.h"
#include "fs/location.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luciole {
  struct AnimationDecoder;

  struct AnimationLimits {
    std::size_t budgetBytes = 64U * 1024U * 1024U; // decoded frames kept at once, the one shown included
    std::size_t maxFrames = 5000;                  // frames read from one play; a longer play ends there
  };

  // Frames kept decoded ahead while an animation longer than its budget streams, and the most frames one decoding
  // step publishes at once.
  inline constexpr std::size_t kAnimationFramesAhead = 4;

  // The size in pixels frames of a sourceWidth by sourceHeight image are decoded at for a maxWidth by maxHeight box:
  // the source's shape, at least one pixel a side, never above the source or the box, and never over a quarter of
  // `budgetBytes`.
  std::pair<int, int>
  animationFrameSize(int sourceWidth, int sourceHeight, int maxWidth, int maxHeight, std::size_t budgetBytes);

  struct AnimationFrame {
    std::vector<std::uint8_t> rgba; // straight alpha, width * height * 4 bytes
    int width = 0;
    int height = 0;
    std::chrono::milliseconds delay{0}; // how long the frame shows; a file's 10 ms or less is 100 ms
    std::size_t index = 0;              // position in its play
  };

  enum class AnimationState : std::uint8_t {
    Opening,  // reading the first frames
    Still,    // one frame: an ordinary image
    Animated, // frames are coming
    Failed,   // nothing to show; message() says why
  };

  // The frames of an animated image, decoded on workers by glycin's sandboxed loaders and scaled to fit a box.
  // A play that fits the budget is kept whole; a longer one is decoded a few frames ahead of the one shown and the
  // file is opened again for each play. Main context only.
  class AnimationFrames {
  public:
    // Starts reading `file`. Frames fit within maxWidth by maxHeight pixels (at least one each) and never exceed
    // the image's own size or a quarter of the budget. `changed` runs on the main context whenever state() changes
    // or new frames arrive, never from inside a call on this object; it may destroy this object.
    AnimationFrames(
        Location file, int maxWidth, int maxHeight, std::function<void()> changed, AnimationLimits limits = {}
    );
    // Cancels decoding: a worker stops after its current frame and publishes nothing.
    ~AnimationFrames();

    AnimationFrames(const AnimationFrames&) = delete;
    AnimationFrames& operator=(const AnimationFrames&) = delete;

    AnimationState state() const noexcept { return m_state; }
    // Why the file failed, or why the animation ended early.
    const std::string& message() const noexcept { return m_message; }
    // The image's own size in pixels once a frame was decoded; zero before, and for a PNG or WebP whose first bytes
    // show it still, which is answered without a decoder.
    int sourceWidth() const noexcept { return m_sourceWidth; }
    int sourceHeight() const noexcept { return m_sourceHeight; }
    // Times the animation plays, as the file says; 0 forever.
    std::uint32_t plays() const noexcept { return m_plays; }
    // Frames in one play, once a play was decoded to its end.
    std::optional<std::size_t> frameCount() const noexcept { return m_frameCount; }
    // The whole play is kept and nothing more is decoded.
    bool resident() const noexcept { return m_resident; }
    // A play ended at a decoding error or at the frame limit.
    bool truncated() const noexcept { return m_truncated; }
    // Decoded bytes kept now, the frame last handed out included.
    std::size_t heldBytes() const noexcept { return (m_frames.size() + (m_shown != nullptr ? 1 : 0)) * m_frameBytes; }
    bool decoding() const noexcept { return m_decoding; }
    // Every play was handed out, or decoding stopped for good and the frames ran out.
    bool finished() const noexcept { return m_finished; }

    // The next frame in play order, starting with the first; empty while it is still decoding and once finished.
    std::shared_ptr<const AnimationFrame> next();

  private:
    struct FillResult;
    using FramePtr = std::shared_ptr<const AnimationFrame>;

    void startFill(std::size_t frames, std::size_t bytes, bool loop);
    void maybeFill();
    void apply(FillResult result);
    void notify();
    void fail(std::string message);
    void stopDecoding();
    void startStreaming();

    std::function<void()> m_changed;
    AnimationLimits m_limits;
    CancelToken m_token;
    std::shared_ptr<int> m_lifetime;
    std::unique_ptr<AnimationDecoder> m_decoder; // while no worker holds it
    AnimationState m_state = AnimationState::Opening;
    std::string m_message;
    std::deque<FramePtr> m_frames; // the first play from its start, or the frames ahead while streaming
    std::size_t m_cursor = 0;      // next frame of m_frames to hand out
    FramePtr m_shown;              // last frame handed out while streaming
    std::size_t m_frameBytes = 0;  // every frame's size
    std::uint32_t m_plays = 0;
    std::uint32_t m_play = 0; // plays begun
    std::optional<std::size_t> m_frameCount;
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    bool m_decoding = false;
    bool m_streaming = false;
    bool m_resident = false;
    bool m_truncated = false;
    bool m_finished = false;
  };
} // namespace luciole
