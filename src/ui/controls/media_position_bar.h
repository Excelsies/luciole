#pragma once

#include "media/media_player.h"
#include "render/scene/node.h"

#include <functional>
#include <optional>

class InputArea;
class RectNode;

// A thin track with the played part filled, as tall as the row it sits in. While enabled, a press seeks, a drag
// follows the pointer and the release lands; a cancelled drag lands where it last was.
class MediaPositionBar final : public Node {
public:
  static constexpr float kTrackHeight = 4;

  // The share of the track asked for, how precisely, and whether the pointer was released.
  std::function<void(double fraction, luciole::SeekMode mode, bool done)> onSeek;

  MediaPositionBar();

  // The played share, clamped to 0 to 1; repaints only when the fill moves by half a pixel or more.
  void setFraction(double fraction);
  void setEnabled(bool enabled) noexcept { m_enabled = enabled; }
  void setRowHeight(float height);

  // The area that takes the pointer.
  InputArea& inputArea() const noexcept { return *m_area; }
  double fraction() const noexcept { return m_fraction; }
  bool enabled() const noexcept { return m_enabled; }

private:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;
  void seekTo(float x, luciole::SeekMode mode, bool done);

  RectNode* m_track = nullptr;
  RectNode* m_fill = nullptr;
  InputArea* m_area = nullptr;
  double m_fraction = 0;
  std::optional<double> m_held; // the last share of a drag in progress
  float m_rowHeight = 0;
  bool m_enabled = false;
};
