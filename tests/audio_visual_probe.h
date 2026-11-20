#pragma once

#include "audio_visual_rig.h"
#include "ui/controls/audio_visual.h"
#include "workbench/audio_frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

// What the tests of the line visuals use beside the rig: a visual that passes another one changed frames or a
// changed palette, and pixel measures the rig does not have.
namespace test::audio {
  // Forwards to `inner`, with the bass held at 1 and no hit when `steadyBass` is set, every band 0 when `quietBands`
  // is set, advances dropped after `advances` of them when that is not negative, and `palette` in place of the
  // host's when it is set.
  class ProbeVisual final : public AudioVisual {
  public:
    struct Options {
      bool steadyBass = false;
      bool quietBands = false;
      int advances = -1;
      std::optional<AudioVisualPalette> palette = std::nullopt;
    };

    ProbeVisual(AudioVisual& inner, Options options) : m_inner(inner), m_options(std::move(options)) {}

    void reset() override { m_inner.reset(); }

    void advance(const luciole::workbench::AudioFrame& frame) override {
      if (m_options.advances >= 0 && m_advanced >= m_options.advances) {
        return;
      }
      ++m_advanced;
      auto changed = frame;
      if (m_options.steadyBass) {
        changed.bass = 1;
        changed.hit = false;
      }
      if (m_options.quietBands) {
        changed.bands = m_quiet;
      }
      m_inner.advance(changed);
    }

    void draw(const AudioVisualDraw& draw) override {
      auto changed = draw;
      if (m_options.palette) {
        changed.palette = *m_options.palette;
      }
      m_inner.draw(changed);
    }

    void release() override { m_inner.release(); }
    void abandon() noexcept override { m_inner.abandon(); }

  private:
    AudioVisual& m_inner;
    Options m_options;
    int m_advanced = 0;
    std::array<float, luciole::workbench::kAudioBandCount> m_quiet{};
  };

  // The box around the pixels with luma at least `minLuma`, right and bottom excluded; empty when none.
  struct PixelBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
  };
  inline PixelBox brightBox(const GlPixels& pixels, int minLuma) {
    PixelBox box{.left = pixels.width, .top = pixels.height, .right = 0, .bottom = 0};
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        if (luma(pixels.at(x, y)) >= minLuma) {
          box = {std::min(box.left, x), std::min(box.top, y), std::max(box.right, x + 1), std::max(box.bottom, y + 1)};
        }
      }
    }
    return box.right > box.left ? box : PixelBox{};
  }

  // The summed luma of `region`.
  inline double lumaSum(const GlPixels& pixels, Region region) {
    double sum = 0;
    for (int y = std::max(region.top, 0); y < std::min(region.bottom, pixels.height); ++y) {
      for (int x = std::max(region.left, 0); x < std::min(region.right, pixels.width); ++x) {
        sum += luma(pixels.at(x, y));
      }
    }
    return sum;
  }

  // The root mean square distance of the pixels with luma at least `minLuma` from (centerX, centerY).
  inline double brightSpread(const GlPixels& pixels, int minLuma, double centerX, double centerY) {
    double sum = 0;
    int count = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        if (luma(pixels.at(x, y)) >= minLuma) {
          sum += ((x - centerX) * (x - centerX)) + ((y - centerY) * (y - centerY));
          ++count;
        }
      }
    }
    return count > 0 ? std::sqrt(sum / count) : 0;
  }

  // The pixels unlike `background` whose mirror image across the vertical middle, and its neighbors on either
  // side, are all like it.
  inline int unmirroredPixels(const GlPixels& pixels, const Pixel& background, int tolerance) {
    const auto lit = [&](int x, int y) {
      if (x < 0 || x >= pixels.width) {
        return false;
      }
      const auto p = pixels.at(x, y);
      return std::abs(p[0] - background[0]) > tolerance
          || std::abs(p[1] - background[1]) > tolerance
          || std::abs(p[2] - background[2]) > tolerance;
    };
    int count = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        const int mirror = pixels.width - 1 - x;
        if (lit(x, y) && !lit(mirror - 1, y) && !lit(mirror, y) && !lit(mirror + 1, y)) {
          ++count;
        }
      }
    }
    return count;
  }
} // namespace test::audio
