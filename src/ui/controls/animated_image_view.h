#pragma once

#include "core/timer_manager.h"
#include "fs/location.h"
#include "imaging/animation_frames.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class ImageNode;
namespace luciole {
  class PreviewFrame;
}
class InputArea;
struct LoadedImageFile;

// Shows a picture in its box: an animated image it plays, or a still the owner decoded. The picture is framed like
// the side preview's: rounded, outlined and shadowed, with room for the shadow kept inside the view's own edge, and
// the visible part stays rounded when the picture is cut. At scale 1 the picture keeps its aspect, fits the box and
// is never drawn larger than its own size in logical pixels; another scale draws it that many times as large,
// centered while it fits. A picture larger than the box scrolls with the wheel and pans when
// dragged, and a new scale keeps the point at the box's center in place. Each frame replaces the texture in place on
// a timer; a click pauses and resumes. Hidden by setShown(false) or by an invisible ancestor, or destroyed, it stops
// its timer and decoding and frees its texture; shown again, it plays from the start.
class AnimatedImageView final : public Node {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr float kMinScale = 0.1F;
  static constexpr float kMaxScale = 4.0F;

  // `clock` gives the time frames are shown at; `limits` bound the decoded frames.
  // `surface` is the role of the background the view sits on; the picture's frame rounds into it.
  explicit AnimatedImageView(
      ColorRole surface, std::function<Clock::time_point()> clock = Clock::now, luciole::AnimationLimits limits = {}
  );
  ~AnimatedImageView() override;

  // The file to play, or none.
  void setFile(std::optional<luciole::Location> file);
  // A still picture in place of the file; its source size is the picture's own size. Null shows nothing.
  void setStill(std::shared_ptr<const LoadedImageFile> pixels);
  void setShown(bool shown);
  // Clamped to kMinScale to kMaxScale; a scale that is not finite and positive is ignored.
  void setPreviewScale(float scale);
  // Logical pixels, clamped to the picture.
  void scrollTo(float x, float y);

  // Opening until the file's first frames are read, and without a file.
  luciole::AnimationState state() const noexcept;
  // The frame on screen.
  std::optional<std::size_t> frameIndex() const noexcept;
  // A frame or the still is on screen.
  bool pictureShown() const noexcept { return m_texture.valid(); }
  bool paused() const noexcept { return m_paused; }
  // The frame timer runs.
  bool playing() const noexcept { return m_frameTimer.active(); }
  bool decoding() const noexcept { return m_frames != nullptr && m_frames->decoding(); }
  // The image's own width and height in pixels; zero until its first frames are read or a still is set.
  std::pair<int, int> sourceSize() const noexcept { return {m_sourceWidth, m_sourceHeight}; }
  float previewScale() const noexcept { return m_scale; }
  float scrollX() const noexcept { return m_scrollX; }
  float scrollY() const noexcept { return m_scrollY; }
  // 0 while the picture fits that way.
  float maxScrollX() const noexcept;
  float maxScrollY() const noexcept;
  const ImageNode& image() const noexcept { return *m_image; }
  InputArea& inputArea() const noexcept { return *m_area; }

  // The file turned out still, failed or animated. Still and Failed show nothing here: the owner shows its
  // ordinary preview instead. Runs on the main context, never from inside a call on this view, and the owner may
  // destroy the view from it.
  std::function<void(luciole::AnimationState)> onStateChanged;

protected:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }

private:
  void start();
  void release();
  void clearFrame();
  void framesChanged();
  void advance();
  void show(std::shared_ptr<const luciole::AnimationFrame> frame);
  void showStill();
  void togglePause();
  void press(float x, float y, bool pressed);
  void drag(float x, float y);
  bool onScreen() const;
  bool needsRestart() const;
  // Room around the box for the picture's shadow, and the box the picture is seen in.
  float inset() const noexcept;
  std::pair<float, float> boxSize() const noexcept;
  // The picture's size in logical pixels at the current scale; the box until the source size is known.
  std::pair<float, float> shownSize() const;

  std::function<Clock::time_point()> m_clock;
  luciole::AnimationLimits m_limits;
  std::optional<luciole::Location> m_file;
  std::unique_ptr<luciole::AnimationFrames> m_frames;
  std::shared_ptr<const luciole::AnimationFrame> m_current;
  std::shared_ptr<const LoadedImageFile> m_still;
  luciole::AnimationState m_reported = luciole::AnimationState::Opening;
  luciole::PreviewFrame* m_frame = nullptr;
  ImageNode* m_image = nullptr;
  InputArea* m_area = nullptr;
  Renderer* m_renderer = nullptr;
  TextureHandle m_texture{};
  Timer m_frameTimer;
  Timer m_startTimer;
  Clock::time_point m_due; // when the frame on screen ends
  Clock::time_point m_pausedAt;
  int m_boxWidth = 0; // device pixels frames are decoded for
  int m_boxHeight = 0;
  int m_sourceWidth = 0; // the picture's own size, once known
  int m_sourceHeight = 0;
  float m_scale = 1;
  float m_scrollX = 0;
  float m_scrollY = 0;
  std::optional<std::pair<float, float>> m_anchor; // the picture's point, in fractions, to keep at the center
  float m_dragX = 0;
  float m_dragY = 0;
  bool m_dragging = false;
  bool m_dragged = false; // the last press moved the picture, so its click does not pause
  bool m_shown = true;
  bool m_paused = false;
  bool m_stalled = false; // the next frame was still decoding when it was due
};
