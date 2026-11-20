#pragma once

#include "audio_visual_rig.h"
#include "test_check.h"
#include "ui/controls/audio_visual.h"

#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <memory>
#include <print>
#include <string_view>
#include <vector>

// The checks of the AudioVisual contract shared by the Eclipse and Dark Sun tests: nothing made for a rectangle
// without area, every GL object freed by release, hostile frames, same frames same pixels, opacity, processor time,
// and the captures at every size.
namespace test::audio::disc {
  using MakeVisual = std::unique_ptr<AudioVisual> (*)();

  // A tiny rectangle for frames that only move a visual's state forward.
  inline constexpr AudioVisualTarget kWarmTarget{.width = 24, .height = 24, .scale = 1};

  // GL names up to kNames that are a program, a shader, a texture or a buffer, one flag each.
  inline std::vector<bool> liveNames() {
    constexpr GLuint kNames = 2048;
    std::vector<bool> live;
    live.reserve(kNames);
    for (GLuint name = 1; name <= kNames; ++name) {
      live.push_back(
          glIsProgram(name) == GL_TRUE
          || glIsShader(name) == GL_TRUE
          || glIsTexture(name) == GL_TRUE
          || glIsBuffer(name) == GL_TRUE
      );
    }
    return live;
  }

  // Moves `visual` through `frames` frames of `source`, all but the last in a tiny rectangle, and draws the last at
  // `target`.
  inline AudioVisualCapture settle(
      AudioVisualRig& rig, AudioVisual& visual, AudioFrameSource& source, int frames, const AudioVisualTarget& target
  ) {
    if (frames > 1) {
      const auto warm = rig.run(visual, source, frames - 1, kWarmTarget);
      TEST_CHECK(warm.stateKept && warm.insideOnly);
    }
    auto capture = rig.run(visual, source, 1, target);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    return capture;
  }

  inline void drawsNothingWithoutArea(AudioVisualRig& rig, MakeVisual make) {
    const auto before = liveNames();
    auto visual = make();
    AudioFrameSource source(AudioScene::Tone);
    const auto capture = rig.run(*visual, source, 5, {.width = 0, .height = 0, .scale = 1});
    TEST_CHECK(capture.stateKept && capture.insideOnly && capture.pixels.rgba.empty());
    TEST_CHECK(liveNames() == before);
    visual->release();
  }

  // release() frees every object, and the next draw makes them again from the state kept: the same picture as a
  // visual that was never released.
  inline void releaseFreesEverything(AudioVisualRig& rig, MakeVisual make) {
    const AudioVisualTarget size{.width = 240, .height = 180, .scale = 1};
    const auto before = liveNames();
    auto released = make();
    auto kept = make();
    AudioFrameSource releasedSource(AudioScene::Noise);
    AudioFrameSource keptSource(AudioScene::Noise);
    rig.run(*released, releasedSource, 30, size);
    TEST_CHECK(liveNames() != before);
    released->release();
    TEST_CHECK(liveNames() == before);
    released->release();
    TEST_CHECK(liveNames() == before);
    const auto again = rig.run(*released, releasedSource, 1, size);
    rig.run(*kept, keptSource, 30, size);
    const auto continued = rig.run(*kept, keptSource, 1, size);
    TEST_CHECK(differingPixels(again.pixels, continued.pixels, 0) == 0);
    released->release();
    kept->release();
    TEST_CHECK(liveNames() == before);
  }

  // abandon() frees nothing; the next draw makes new objects and gives the same picture.
  inline void abandonFreesNothing(AudioVisualRig& rig, MakeVisual make) {
    const AudioVisualTarget size{.width = 240, .height = 180, .scale = 1};
    auto visual = make();
    auto kept = make();
    AudioFrameSource source(AudioScene::Tone);
    AudioFrameSource keptSource(AudioScene::Tone);
    rig.run(*visual, source, 10, size);
    const auto drawn = liveNames();
    visual->abandon();
    TEST_CHECK(liveNames() == drawn);
    const auto again = rig.run(*visual, source, 1, size);
    const auto remade = liveNames();
    TEST_CHECK(std::ranges::count(remade, true) > std::ranges::count(drawn, true));
    rig.run(*kept, keptSource, 10, size);
    const auto continued = rig.run(*kept, keptSource, 1, size);
    TEST_CHECK(differingPixels(again.pixels, continued.pixels, 0) == 0);
    visual->release();
    kept->release();
  }

  // Frames that break every promise draw without a GL error and keep the state, and a reset gives a new visual's
  // picture.
  inline void survivesHostileFrames(AudioVisualRig& rig, MakeVisual make) {
    const AudioVisualTarget size{.width = 300, .height = 200, .scale = 1.5F};
    auto visual = make();
    AudioFrameSource hostile(AudioScene::Hostile);
    const auto capture = rig.run(*visual, hostile, 60, size);
    TEST_CHECK(capture.stateKept && capture.insideOnly);
    visual->reset();
    auto fresh = make();
    AudioFrameSource first(AudioScene::Silence);
    AudioFrameSource second(AudioScene::Silence);
    const auto afterReset = rig.run(*visual, first, 1, size);
    const auto newVisual = rig.run(*fresh, second, 1, size);
    TEST_CHECK(differingPixels(afterReset.pixels, newVisual.pixels, 0) == 0);
    visual->release();
    fresh->release();
  }

  inline void sameFramesSamePixels(AudioVisualRig& rig, MakeVisual make) {
    const AudioVisualTarget size{.width = 450, .height = 600, .scale = 1.5F};
    auto first = make();
    auto second = make();
    AudioFrameSource firstSource(AudioScene::Noise);
    AudioFrameSource secondSource(AudioScene::Noise);
    const auto a = settle(rig, *first, firstSource, 90, size);
    const auto b = settle(rig, *second, secondSource, 90, size);
    TEST_CHECK(differingPixels(a.pixels, b.pixels, 0) == 0);
    first->release();
    second->release();
  }

  // At opacity 0 the rectangle keeps its background.
  inline void opacityScalesEverything(AudioVisualRig& rig, MakeVisual make) {
    auto visual = make();
    AudioFrameSource source(AudioScene::Kicks);
    const auto capture = rig.run(*visual, source, 20, {.width = 300, .height = 300, .scale = 1, .opacity = 0});
    const auto background = pixelOf(rigPalette().background);
    TEST_CHECK(pixelsOtherThan(capture.pixels, background, 0, wholeRegion(capture.pixels)).count == 0);
    visual->release();
  }

  // Advancing and drawing one frame takes well under half a millisecond of processor time at every size.
  inline void staysCheap(AudioVisualRig& rig, MakeVisual make, std::string_view name) {
    for (const auto& size : kCaptureTargets) {
      auto visual = make();
      AudioFrameSource source(AudioScene::Kicks);
      const auto capture = rig.run(*visual, source, 120, size);
      TEST_CHECK(capture.stateKept && capture.insideOnly);
      std::println(
          "{} {}x{} at {}: {:.4f} ms mean, {:.4f} ms max processor time per frame", name, size.width, size.height,
          size.scale, capture.meanCpuMilliseconds, capture.maxCpuMilliseconds
      );
      TEST_CHECK(capture.meanCpuMilliseconds < 0.5);
      visual->release();
    }
  }

  // Each scene after a few seconds, at every capture size, written to LUCIOLE_CAPTURE_DIR when it is set. Kicks
  // stop eight frames after a hit.
  inline void capturesEveryScene(AudioVisualRig& rig, MakeVisual make, std::string_view name) {
    struct Scene {
      AudioScene scene;
      std::string_view label;
      int frames;
    };
    constexpr std::array<Scene, 4> kScenes{{
        {.scene = AudioScene::Tone, .label = "tone", .frames = 180},
        {.scene = AudioScene::Noise, .label = "noise", .frames = 180},
        {.scene = AudioScene::Kicks, .label = "kicks", .frames = 159},
        {.scene = AudioScene::Silence, .label = "silence", .frames = 180},
    }};
    for (const auto& scene : kScenes) {
      for (const auto& size : kCaptureTargets) {
        auto visual = make();
        AudioFrameSource source(scene.scene);
        const auto capture = settle(rig, *visual, source, scene.frames, size);
        keepForViewing(capture.pixels, captureName(name, scene.label, size));
        visual->release();
      }
    }
  }

  inline void checkContract(AudioVisualRig& rig, MakeVisual make, std::string_view name) {
    drawsNothingWithoutArea(rig, make);
    releaseFreesEverything(rig, make);
    abandonFreesNothing(rig, make);
    survivesHostileFrames(rig, make);
    sameFramesSamePixels(rig, make);
    opacityScalesEverything(rig, make);
    staysCheap(rig, make, name);
    capturesEveryScene(rig, make, name);
  }
} // namespace test::audio::disc
