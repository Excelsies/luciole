#pragma once

#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"

#include <GLES2/gl2.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// Checks of the AudioVisual contract that every visual's test runs: its output stays in its rectangle and leaves
// the GL state alone at every capture size, hostile frames and empty rectangles, identical runs, a redraw without an
// advance, the processor time of a frame (held only by the benchmark), and the GL objects it frees.
namespace test::audio {
  using VisualFactory = std::function<std::unique_ptr<AudioVisual>()>;

  // The names of every live program, buffer and texture up to a bound well above what the tests create.
  inline std::vector<GLuint> liveGlObjects() {
    constexpr GLuint kLastName = 4096;
    std::vector<GLuint> live;
    for (GLuint name = 1; name <= kLastName; ++name) {
      if (glIsProgram(name) == GL_TRUE) {
        live.push_back(name);
      }
      if (glIsBuffer(name) == GL_TRUE) {
        live.push_back(name + kLastName);
      }
      if (glIsTexture(name) == GL_TRUE) {
        live.push_back(name + (2 * kLastName));
      }
    }
    return live;
  }

  // The summed distance of every pixel's red, green and blue from the background's.
  inline double distanceFromBackground(const GlPixels& pixels) {
    const auto background = pixelOf(rigPalette().background);
    double sum = 0;
    for (int y = 0; y < pixels.height; ++y) {
      for (int x = 0; x < pixels.width; ++x) {
        const auto p = pixels.at(x, y);
        for (std::size_t c = 0; c < 3; ++c) {
          sum += std::abs(p.at(c) - background.at(c));
        }
      }
    }
    return sum;
  }

  // Draws `visual` again at `target` without an advance, the way the rig places it, and reads its rectangle back.
  inline GlPixels redrawWithoutAdvance(AudioVisual& visual, const AudioVisualTarget& target) {
    const auto width = static_cast<int>(std::lround(target.width * target.scale));
    const auto height = static_cast<int>(std::lround(target.height * target.scale));
    const AudioVisualRect rect{
        .x = AudioVisualRig::kMargin,
        .y = AudioVisualRig::kHeight - AudioVisualRig::kMargin - height,
        .width = width,
        .height = height,
    };
    const AudioVisualDraw draw{
        .node =
            CustomDrawContext{
                .surfaceWidth = AudioVisualRig::kWidth / target.scale,
                .surfaceHeight = AudioVisualRig::kHeight / target.scale,
                .pixelScaleX = target.scale,
                .pixelScaleY = target.scale,
                .transform =
                    Mat3::translation(AudioVisualRig::kMargin / target.scale, AudioVisualRig::kMargin / target.scale),
                .opacity = target.opacity,
                .width = static_cast<float>(target.width),
                .height = static_cast<float>(target.height),
            },
        .rect = rect,
        .scale = target.scale,
        .palette = rigPalette(),
    };
    const Color& background = rigPalette().background;
    glScissor(rect.x, rect.y, rect.width, rect.height);
    glClearColor(background.r, background.g, background.b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(0, 0, AudioVisualRig::kWidth, AudioVisualRig::kHeight);
    visual.draw(draw);
    TEST_CHECK(glGetError() == GL_NO_ERROR);
    const auto row = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> bottomUp(row * static_cast<std::size_t>(height));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(rect.x, rect.y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    GlPixels pixels{.width = width, .height = height, .rgba = std::vector<std::uint8_t>(bottomUp.size())};
    for (std::size_t y = 0; y < static_cast<std::size_t>(height); ++y) {
      std::memcpy(
          pixels.rgba.data() + (y * row), bottomUp.data() + ((static_cast<std::size_t>(height) - 1 - y) * row), row
      );
    }
    return pixels;
  }

  // At every capture size, after `frames` of `scene`: the state kept, nothing outside, the frame's processor time
  // within bounds. Keeps each picture for viewing.
  inline void checkCaptureSizes(
      AudioVisualRig& rig, const VisualFactory& make, std::string_view name, AudioScene scene,
      std::string_view sceneName, int frames
  ) {
    for (const auto& target : kCaptureTargets) {
      auto visual = make();
      AudioFrameSource source(scene);
      const auto capture = rig.run(*visual, source, frames, target);
      TEST_CHECK(capture.stateKept);
      TEST_CHECK(capture.insideOnly);
      std::println(
          "{} {} {}x{} at {}: {:.4f} ms mean, {:.4f} ms max processor time per frame", name, sceneName, target.width,
          target.height, target.scale, capture.meanCpuMilliseconds, capture.maxCpuMilliseconds
      );
      TEST_CHECK(withinCpuBudget(capture.meanCpuMilliseconds));
      keepForViewing(capture.pixels, captureName(name, sceneName, target));
      visual->release();
    }
  }

  // Hostile frames keep the state and the rectangle, and an empty rectangle draws nothing.
  inline void checkHostileAndEmpty(AudioVisualRig& rig, const VisualFactory& make) {
    auto visual = make();
    AudioFrameSource hostile(AudioScene::Hostile);
    const auto capture = rig.run(*visual, hostile, 16, {.width = 320, .height = 240, .scale = 1.5F});
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    for (const auto& empty :
         {AudioVisualTarget{.width = 0, .height = 0, .scale = 1}, AudioVisualTarget{.width = 300, .height = 0}}) {
      const auto nothing = rig.run(*visual, hostile, 4, empty);
      TEST_CHECK(nothing.pixels.rgba.empty() && nothing.stateKept && nothing.insideOnly);
    }
    // After all that, a calm scene draws as it would have.
    AudioFrameSource tone(AudioScene::Tone);
    visual->reset();
    const auto after = rig.run(*visual, tone, 30, {.width = 320, .height = 240, .scale = 1});
    auto fresh = make();
    AudioFrameSource freshTone(AudioScene::Tone);
    const auto clean = rig.run(*fresh, freshTone, 30, {.width = 320, .height = 240, .scale = 1});
    TEST_CHECK(differingPixels(after.pixels, clean.pixels, 0) == 0);
    visual->release();
    fresh->release();
  }

  // Two runs from the same frames give the same pixels, a redraw without an advance gives them again, a half
  // opacity dims them, and release frees every GL object the visual made, also after it was abandoned once.
  inline void checkRunsAndObjects(AudioVisualRig& rig, const VisualFactory& make) {
    const AudioVisualTarget target{.width = 360, .height = 280, .scale = 1.5F};
    const auto before = liveGlObjects();
    auto first = make();
    auto second = make();
    AudioFrameSource firstSource(AudioScene::Noise);
    AudioFrameSource secondSource(AudioScene::Noise);
    const auto a = rig.run(*first, firstSource, 45, target);
    const auto b = rig.run(*second, secondSource, 45, target);
    TEST_CHECK(differingPixels(a.pixels, b.pixels, 0) == 0);
    TEST_CHECK(differingPixels(a.pixels, redrawWithoutAdvance(*first, target), 0) == 0);
    TEST_CHECK(liveGlObjects().size() > before.size());

    second->release();
    first->abandon();
    const auto abandoned = liveGlObjects();
    AudioFrameSource dimSource(AudioScene::Noise);
    const auto dim = rig.run(*first, dimSource, 1, {.width = 360, .height = 280, .scale = 1.5F, .opacity = 0.5F});
    TEST_CHECK(dim.stateKept && dim.insideOnly);
    TEST_CHECK(liveGlObjects().size() > abandoned.size());
    first->release();
    TEST_CHECK(liveGlObjects() == abandoned);

    // Half the host's opacity halves how far every pixel moves from the background.
    auto full = make();
    auto half = make();
    AudioFrameSource fullSource(AudioScene::Tone);
    AudioFrameSource halfSource(AudioScene::Tone);
    const auto shown = rig.run(*full, fullSource, 20, {.width = 300, .height = 200});
    const auto faded = rig.run(*half, halfSource, 20, {.width = 300, .height = 200, .opacity = 0.5F});
    const double fullLight = distanceFromBackground(shown.pixels);
    TEST_CHECK(fullLight > 0);
    const double ratio = distanceFromBackground(faded.pixels) / fullLight;
    std::println("half opacity keeps {:.3f} of the light", ratio);
    TEST_CHECK(ratio > 0.4 && ratio < 0.6);
    full->release();
    half->release();
  }
} // namespace test::audio
